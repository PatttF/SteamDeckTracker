#include "LV2Instrument.h"
#include "CommandList.h"
#include "System/Console/Trace.h"
#include "Application/Mixer/MixerService.h"
#include <string.h>
#include <cmath>
#include <map>
#include <cstdlib>
#include <algorithm>
#include <lilv/lilv.h>
#include <lv2/urid/urid.h>
#include <lv2/atom/atom.h>
#include <lv2/midi/midi.h>

// LV2 URI definitions
#define LV2_ATOM__Sequence "http://lv2plug.in/ns/ext/atom#Sequence"
#define LV2_MIDI__MidiEvent "http://lv2plug.in/ns/ext/midi#MidiEvent"

// Global URID map for this instance
static std::map<std::string, LV2_URID> g_uridMap;
static LV2_URID g_nextUrid = 1;

static LV2_URID urid_map(LV2_URID_Map_Handle handle, const char* uri) {
    auto it = g_uridMap.find(uri);
    if (it != g_uridMap.end()) {
        return it->second;
    }
    LV2_URID urid = g_nextUrid++;
    g_uridMap[uri] = urid;
    return urid;
}

static const char* urid_unmap(LV2_URID_Unmap_Handle handle, LV2_URID urid) {
    for (auto& pair : g_uridMap) {
        if (pair.second == urid) {
            return pair.first.c_str();
        }
    }
    return nullptr;
}

// LV2 features
static LV2_URID_Map g_uridMapFeature = { nullptr, urid_map };
static LV2_URID_Unmap g_uridUnmapFeature = { nullptr, urid_unmap };

static LV2_Feature g_mapFeature = { LV2_URID__map, &g_uridMapFeature };
static LV2_Feature g_unmapFeature = { LV2_URID__unmap, &g_uridUnmapFeature };
static const LV2_Feature* g_features[] = { &g_mapFeature, &g_unmapFeature, nullptr };

// Cached URIDs for performance
static LV2_URID g_midiEventUrid = 0;
static LV2_URID g_atomSequenceUrid = 0;

LV2Instrument::LV2Instrument() {
    strcpy(name_, "LV2");
    pluginURI_[0] = '\0';
    world_ = nullptr;
    plugin_ = nullptr;
    pluginInstance_ = nullptr;
    audioBufferL_ = nullptr;
    audioBufferR_ = nullptr;
    audioInputL_ = nullptr;
    audioInputR_ = nullptr;
    bufferSize_ = 0;
    audioInputPortL_ = -1;
    audioInputPortR_ = -1;
    audioOutputPortL_ = -1;
    audioOutputPortR_ = -1;
    midiInputPort_ = -1;
    midiBuffer_ = nullptr;
    midiBufferSize_ = 0;
    isActivated_ = false;

    // Initialize channel state
    for (int i = 0; i < SONG_CHANNEL_COUNT; i++) {
        lastNote_[i] = -1;
        playing_[i] = false;
        // Initialize reverb state
        memset(reverbBuffer_[i], 0, sizeof(reverbBuffer_[i]));
        reverbDecay_[i] = 0;
        reverbSend_[i] = 0;
        reverbPos_[i] = 0;
    }

    // Setup variables
    // Use a string variable for plugin URI (empty = no plugin)
    Variable *v = new Variable("plugin", LV2IP_PLUGIN, "");
    Insert(v);
    v = new Variable("volume", LV2IP_VOLUME, 255);
    Insert(v);
    v = new Variable("pan", LV2IP_PAN, 0x7F);
    Insert(v);
    v = new Variable("table", LV2IP_TABLE, -1);
    Insert(v);
    v = new Variable("table automation", LV2IP_TABLEAUTO, false);
    Insert(v);
}

LV2Instrument::~LV2Instrument() {
    Purge();
}

bool LV2Instrument::Init() {
    tableState_.Reset();
    // If a plugin URI was saved in the 'plugin' Variable, load it now so
    // that parameter Variables get created and we can apply any saved values.
    Variable *pv = FindVariable(LV2IP_PLUGIN);
    if (pv) {
        const char *s = pv->GetString();
        // Ignore empty or numeric placeholder values like "-1" or "0"
        if (s && s[0] != '\0') {
            bool allDigits = true;
            const char *p = s;
            if (*p == '-') p++;
            while (*p) { if (!(*p >= '0' && *p <= '9')) { allDigits = false; break; } p++; }
            if (!allDigits) {
                SetPlugin(s);
            }
        }
    }

    // After parameters discovered, apply any pending parameter values loaded
    // earlier from the project file (they were saved before the parameters
    // variables were created).
    for (auto &kv : pendingParamValues_) {
        const std::string &name = kv.first;
        const std::string &val = kv.second;
        Variable *v = nullptr;
        // Find variable by name
        IteratorPtr<Variable> it(GetIterator());
        for (it->Begin(); !it->IsDone(); it->Next()) {
            Variable &vv = it->CurrentItem();
            if (name == vv.GetName()) {
                v = &vv;
                break;
            }
        }
        if (v) {
            v->SetString(val.c_str(), false);
        }
    }
    pendingParamValues_.clear();
    return true;
}

void LV2Instrument::OnStart() {
    tableState_.Reset();
}

bool LV2Instrument::Start(int channel, unsigned char note, bool retrigger) {
    if (IsEmpty()) {
        return false;
    }

    // Stop any existing note on this channel first
    if (playing_[channel] && lastNote_[channel] >= 0) {
        MidiEvent noteOff;
        noteOff.data[0] = 0x80;  // Note Off
        noteOff.data[1] = lastNote_[channel];
        noteOff.data[2] = 0;
        noteOff.size = 3;
        pendingMidiEvents_.push_back(noteOff);
    }

    lastNote_[channel] = note;
    playing_[channel] = true;

    // Queue MIDI note-on event
    MidiEvent noteOn;
    noteOn.data[0] = 0x90;  // Note On
    noteOn.data[1] = note;
    noteOn.data[2] = 100;   // Velocity
    noteOn.size = 3;
    pendingMidiEvents_.push_back(noteOn);

    return true;
}

void LV2Instrument::Stop(int channel) {
    if (!playing_[channel]) {
        return;
    }

    playing_[channel] = false;

    // Queue MIDI note-off event
    if (lastNote_[channel] >= 0) {
        MidiEvent noteOff;
        noteOff.data[0] = 0x80;  // Note Off
        noteOff.data[1] = lastNote_[channel];
        noteOff.data[2] = 0;
        noteOff.size = 3;
        pendingMidiEvents_.push_back(noteOff);
    }
}

bool LV2Instrument::Render(int channel, fixed *buffer, int size, bool updateTick) {
    
    if (IsEmpty() || !playing_[channel]) {
        // Fill with silence
        for (int i = 0; i < size * 2; i++) {
            buffer[i] = 0;
        }
        return false;
    }

    if (!pluginInstance_) {
        // No plugin loaded, output silence
        for (int i = 0; i < size * 2; i++) {
            buffer[i] = 0;
        }
        return true;
    }

    // Allocate buffers if needed (use fixed size of 2048 to avoid constant reallocation)
    const int BUFFER_SIZE = 2048;
    if (!audioBufferL_) {
        audioBufferL_ = new float[BUFFER_SIZE];
        audioBufferR_ = new float[BUFFER_SIZE];
        audioInputL_ = new float[BUFFER_SIZE];
        audioInputR_ = new float[BUFFER_SIZE];
        bufferSize_ = BUFFER_SIZE;
        connectPorts(BUFFER_SIZE);
    }
    
    // Activate plugin once after first connection
    if (!isActivated_ && pluginInstance_) {
        LilvInstance* instance = (LilvInstance*)pluginInstance_;
        lilv_instance_activate(instance);
        isActivated_ = true;
    }

    // Clear input buffers (no audio input for synths)
    for (int i = 0; i < size; i++) {
        audioInputL_[i] = 0.0f;
        audioInputR_[i] = 0.0f;
    }

    // Update parameter values from Variables
    for (size_t i = 0; i < parameters_.size() && i < controlValues_.size(); i++) {
        if (parameters_[i].variable) {
            int scaledValue = parameters_[i].variable->GetInt();
            float realValue = parameters_[i].minValue + 
                (scaledValue / 127.0f) * (parameters_[i].maxValue - parameters_[i].minValue);

            // If enumerated: prefer explicit scale points if present, otherwise snap to integer steps
            if (!parameters_[i].scalePoints.empty()) {
                float closest = parameters_[i].scalePoints[0].value;
                float bestDiff = std::fabs(realValue - closest);
                for (size_t sp = 1; sp < parameters_[i].scalePoints.size(); ++sp) {
                    float v = parameters_[i].scalePoints[sp].value;
                    float d = std::fabs(realValue - v);
                    if (d < bestDiff) {
                        bestDiff = d;
                        closest = v;
                    }
                }
                if (closest != realValue) {
                    Trace::Log("LV2","Snapped to nearest scale point: param=%s:%s from %f to %f", parameters_[i].groupName.c_str(), parameters_[i].name.c_str(), realValue, closest);
                }
                realValue = closest;
            } else if (parameters_[i].isEnumeration) {
                // No explicit labels available, but port is enumerated: round to integer within range
                float rounded = std::round(realValue);
                if (rounded < parameters_[i].minValue) rounded = parameters_[i].minValue;
                if (rounded > parameters_[i].maxValue) rounded = parameters_[i].maxValue;
                if (rounded != realValue) {
                    Trace::Log("LV2","Rounded enumerated param: param=%s:%s from %f to %f", parameters_[i].groupName.c_str(), parameters_[i].name.c_str(), realValue, rounded);
                }
                realValue = rounded;
            }

            // Extra logging for braids shape crashes
            if (parameters_[i].groupName == "braids" && parameters_[i].name.find("Shape") != std::string::npos) {
                Trace::Log("LV2","Braids shape set: port=%d scaled=%d value=%f", parameters_[i].portIndex, scaledValue, realValue);
            }

            controlValues_[i] = realValue;
            parameters_[i].currentValue = realValue;

            // Sync to per-port storage so connected LV2 ports see the updated value immediately
            if (parameters_[i].portIndex >= 0) {
                if ((int)portControlStorage_.size() <= parameters_[i].portIndex) {
                    portControlStorage_.resize(parameters_[i].portIndex + 1, 0.0f);
                }
                portControlStorage_[parameters_[i].portIndex] = realValue;
            }
        }
    }

    // Write pending MIDI events to the atom buffer
    if (midiBuffer_ && midiInputPort_ >= 0 && g_midiEventUrid > 0) {
        // LV2 Atom Sequence structure using proper LV2 atom types
        LV2_Atom_Sequence* seq = (LV2_Atom_Sequence*)midiBuffer_;
        
        // Initialize sequence header
        seq->atom.size = sizeof(LV2_Atom_Sequence_Body);  // Will be updated
        seq->atom.type = g_atomSequenceUrid;
        seq->body.unit = 0;  // Frames
        seq->body.pad = 0;
        
        uint8_t* buf = (uint8_t*)LV2_ATOM_CONTENTS(LV2_Atom_Sequence, seq);
        size_t offset = 0;
        size_t capacity = midiBufferSize_ - sizeof(LV2_Atom_Sequence);
        
        // Write pending MIDI events
        for (size_t i = 0; i < pendingMidiEvents_.size() && offset + 32 < capacity; i++) {
            MidiEvent& evt = pendingMidiEvents_[i];
            
            LV2_Atom_Event* event = (LV2_Atom_Event*)(buf + offset);
            event->time.frames = 0;  // All events at time 0
            event->body.size = evt.size;
            event->body.type = g_midiEventUrid;
            
            // Copy MIDI data
            memcpy(LV2_ATOM_BODY(&event->body), evt.data, evt.size);
            
            // Calculate padded size (atoms must be 64-bit aligned)
            size_t padded_size = sizeof(LV2_Atom_Event) + ((evt.size + 7) & ~7);
            offset += padded_size;
        }
        pendingMidiEvents_.clear();
        
        // Update final sequence size
        seq->atom.size = sizeof(LV2_Atom_Sequence_Body) + offset;
    }

    // Run the plugin to generate output
    LilvInstance* instance = (LilvInstance*)pluginInstance_;

    // Debug: log braids shape parameter(s) before calling into the plugin
    for (size_t i = 0; i < parameters_.size() && i < controlValues_.size(); ++i) {
        if (parameters_[i].groupName == "braids" && parameters_[i].name.find("Shape") != std::string::npos) {
            Trace::Log("LV2","Running plugin with braids shape port=%d value=%f", parameters_[i].portIndex, controlValues_[i]);
        }
    }

    lilv_instance_run(instance, size);

    // Convert float output to fixed point stereo interleaved
    // Apply volume control
    Variable *volVar = FindVariable(LV2IP_VOLUME);
    float volume = volVar ? (volVar->GetInt() / 255.0f) : 1.0f;

    // Plugin audio is now in audioBufferL_ and audioBufferR_

    for (int i = 0; i < size; i++) {
        float l = audioBufferL_[i] * volume;
        float r = audioBufferR_[i] * volume;
        
        // Clamp and convert to fixed point
        if (l > 1.0f) l = 1.0f;
        if (l < -1.0f) l = -1.0f;
        if (r > 1.0f) r = 1.0f;
        if (r < -1.0f) r = -1.0f;
        
        // Convert to fixed-point: audio samples must use i2fp() to shift into proper range
        buffer[i * 2] = i2fp((int)(l * 32767.0f));
        buffer[i * 2 + 1] = i2fp((int)(r * 32767.0f));
    }

    // Apply per-channel reverb effect if enabled
    if (reverbSend_[channel] > 0) {
        fixed *reverbBuf = reverbBuffer_[channel];
        int reverbPos = reverbPos_[channel];
        fixed decay = reverbDecay_[channel];
        fixed send = reverbSend_[channel];
        
        // Tap offsets for early reflections
        const int tapOffsets[4] = {1557, 2801, 4409, 4000};
        const fixed tapGains[4] = {fl2fp(0.35f), fl2fp(0.25f), fl2fp(0.18f), fl2fp(0.12f)};
        
        fixed *outPtr = buffer;
        for (int i = 0; i < size; i++) {
            fixed dryL = *outPtr;
            fixed dryR = *(outPtr + 1);
            
            // Read from delay taps and sum
            fixed wetL = 0;
            fixed wetR = 0;
            for (int t = 0; t < 4; t++) {
                int readPos = reverbPos - tapOffsets[t];
                if (readPos < 0) readPos += LV2_REVERB_BUFFER_LENGTH;
                
                wetL = fp_add(wetL, fp_mul(reverbBuf[readPos * 2], tapGains[t]));
                wetR = fp_add(wetR, fp_mul(reverbBuf[readPos * 2 + 1], tapGains[t]));
            }
            
            // Write input + decayed feedback to delay line
            int fbPos = reverbPos - LV2_REVERB_BUFFER_LENGTH + 100;
            if (fbPos < 0) fbPos += LV2_REVERB_BUFFER_LENGTH;
            fixed fbL = fp_mul(reverbBuf[fbPos * 2], decay);
            fixed fbR = fp_mul(reverbBuf[fbPos * 2 + 1], decay);
            
            reverbBuf[reverbPos * 2] = fp_add(fp_mul(dryL, send), fbL);
            reverbBuf[reverbPos * 2 + 1] = fp_add(fp_mul(dryR, send), fbR);
            
            // Mix wet with dry output
            *outPtr = fp_add(dryL, wetL);
            *(outPtr + 1) = fp_add(dryR, wetR);
            
            outPtr += 2;
            reverbPos++;
            if (reverbPos >= LV2_REVERB_BUFFER_LENGTH) {
                reverbPos = 0;
            }
        }
        reverbPos_[channel] = reverbPos;
    }

    return true;
}

void LV2Instrument::ProcessCommand(int channel, FourCC cc, ushort value) {
    // Handle REVB command for reverb effect
    if (cc == I_CMD_REVB) {
        // REVB:aabb - aa=decay (0-FF), bb=send amount (0-FF)
        unsigned char decayVal = (value >> 8) & 0xFF;
        unsigned char sendAmount = value & 0xFF;
        
        // Set per-channel reverb parameters
        reverbDecay_[channel] = fl2fp((decayVal / 255.0f) * 0.9f);
        reverbSend_[channel] = fl2fp(sendAmount / 255.0f);
        
        Trace::Log("REVERB", "LV2 Channel %d: decay=%d send=%d", channel, decayVal, sendAmount);
    }
    // TODO: Handle other commands like volume, pan, etc.
}

bool LV2Instrument::IsInitialized() {
    return true;
}

const char *LV2Instrument::GetName() {
    if (IsEmpty()) {
        return "-- no plugin --";
    }
    return name_;
}

void LV2Instrument::Purge() {
    // Parameter Variables are owned by the instrument's VariableContainer
    // (inserted via `Insert(variable)`). Do not delete them here to avoid
    // double-free; the container's destructor will free owned Variables.
    cleanupPlugin();
    pluginURI_[0] = '\0';
    strcpy(name_, "LV2");
    parameters_.clear();
    controlValues_.clear();
}

void LV2Instrument::cleanupPlugin() {
    if (pluginInstance_) {
        if (isActivated_) {
            lilv_instance_deactivate((LilvInstance*)pluginInstance_);
            isActivated_ = false;
        }
        lilv_instance_free((LilvInstance*)pluginInstance_);
        pluginInstance_ = nullptr;
    }
    if (audioBufferL_) {
        delete[] audioBufferL_;
        audioBufferL_ = nullptr;
    }
    if (audioBufferR_) {
        delete[] audioBufferR_;
        audioBufferR_ = nullptr;
    }
    if (audioInputL_) {
        delete[] audioInputL_;
        audioInputL_ = nullptr;
    }
    if (audioInputR_) {
        delete[] audioInputR_;
        audioInputR_ = nullptr;
    }
    if (midiBuffer_) {
        delete[] midiBuffer_;
        midiBuffer_ = nullptr;
        midiBufferSize_ = 0;
    }
    if (world_) {
        lilv_world_free((LilvWorld*)world_);
        world_ = nullptr;
    }
    plugin_ = nullptr;
    bufferSize_ = 0;
}

int LV2Instrument::GetTable() {
    Variable *v = FindVariable(LV2IP_TABLE);
    if (v) {
        return v->GetInt();
    }
    return -1;
}

bool LV2Instrument::GetTableAutomation() {
    Variable *v = FindVariable(LV2IP_TABLEAUTO);
    if (v) {
        return v->GetInt() != 0;
    }
    return false;
}

void LV2Instrument::GetTableState(TableSaveState &state) {
    state = tableState_;
}

void LV2Instrument::SetTableState(TableSaveState &state) {
    tableState_ = state;
}

void LV2Instrument::SetPlugin(const char *uri) {
    if (uri && strlen(uri) < 256) {
        strncpy(pluginURI_, uri, 255);
        pluginURI_[255] = '\0';
        
        // Extract plugin name from URI for display
        // TODO: Get actual plugin name from LV2 world
        const char *lastSlash = strrchr(uri, '/');
        if (lastSlash && lastSlash[1] != '\0') {
            strncpy(name_, lastSlash + 1, 79);
            name_[79] = '\0';
        } else {
            strncpy(name_, uri, 79);
            name_[79] = '\0';
        }
        
        // Load the plugin and discover parameters
        loadPlugin();
        discoverParameters();
        // Update the 'plugin' Variable so the value is saved with the project
        Variable *pv = FindVariable(LV2IP_PLUGIN);
        if (pv) pv->SetString(pluginURI_, false);
    }
}

void LV2Instrument::StorePendingVariable(const char *name, const char *value) {
    if (!name || !value) return;
    pendingParamValues_[std::string(name)] = std::string(value);
}

void LV2Instrument::loadPlugin() {
    cleanupPlugin();
    
    if (pluginURI_[0] == '\0') {
        return;
    }
    
    // Create LV2 world
    world_ = lilv_world_new();
    if (!world_) {
        Trace::Error("LV2: Failed to create world");
        return;
    }
    
    LilvWorld* world = (LilvWorld*)world_;
    lilv_world_load_all(world);
    
    // Find the plugin by URI
    LilvNode* uri_node = lilv_new_uri(world, pluginURI_);
    if (!uri_node) {
        Trace::Error("LV2: Invalid plugin URI: %s", pluginURI_);
        cleanupPlugin();
        return;
    }
    
    const LilvPlugin* plugin = lilv_plugins_get_by_uri(lilv_world_get_all_plugins(world), uri_node);
    lilv_node_free(uri_node);
    
    if (!plugin) {
        Trace::Error("LV2: Plugin not found: %s", pluginURI_);
        cleanupPlugin();
        return;
    }
    
    plugin_ = (void*)plugin;
    
    // Initialize URIDs if not already done
    if (g_midiEventUrid == 0) {
        g_midiEventUrid = urid_map(nullptr, LV2_MIDI__MidiEvent);
        g_atomSequenceUrid = urid_map(nullptr, LV2_ATOM__Sequence);
    }
    
    // Instantiate the plugin with 44100 Hz sample rate and URID features
    LilvInstance* instance = lilv_plugin_instantiate(plugin, 44100.0, g_features);
    if (!instance) {
        Trace::Error("LV2: Failed to instantiate plugin: %s", pluginURI_);
        cleanupPlugin();
        return;
    }
    
    pluginInstance_ = instance;
}

void LV2Instrument::discoverParameters() {
    parameters_.clear();
    controlValues_.clear();
    audioInputPortL_ = -1;
    audioInputPortR_ = -1;
    audioOutputPortL_ = -1;
    audioOutputPortR_ = -1;
    midiInputPort_ = -1;
    
    if (!plugin_ || !world_) {
        return;
    }
    
    const LilvPlugin* plugin = (const LilvPlugin*)plugin_;
    LilvWorld* world = (LilvWorld*)world_;
    
    // Get the number of ports
    uint32_t num_ports = lilv_plugin_get_num_ports(plugin);
    
    // Create URIs for port properties we care about
    LilvNode* input_class = lilv_new_uri(world, LILV_URI_INPUT_PORT);
    LilvNode* output_class = lilv_new_uri(world, LILV_URI_OUTPUT_PORT);
    LilvNode* control_class = lilv_new_uri(world, LILV_URI_CONTROL_PORT);
    LilvNode* audio_class = lilv_new_uri(world, LILV_URI_AUDIO_PORT);
    LilvNode* atom_port = lilv_new_uri(world, "http://lv2plug.in/ns/ext/atom#AtomPort");
    LilvNode* midi_event = lilv_new_uri(world, "http://lv2plug.in/ns/ext/midi#MidiEvent");
    
    // Iterate through all ports
    for (uint32_t i = 0; i < num_ports; i++) {
        const LilvPort* port = lilv_plugin_get_port_by_index(plugin, i);
        
        // Check for MIDI atom input port
        if (midiInputPort_ == -1 && 
            lilv_port_is_a(plugin, port, atom_port) &&
            lilv_port_is_a(plugin, port, input_class)) {
            // Check if it supports MIDI events
            LilvNode* buffer_type_uri = lilv_new_uri(world, "http://lv2plug.in/ns/ext/atom#bufferType");
            const LilvNodes* buf_types = lilv_port_get_value(plugin, port, buffer_type_uri);
            if (buf_types) {
                midiInputPort_ = i;
                lilv_nodes_free((LilvNodes*)buf_types);
            }
            lilv_node_free(buffer_type_uri);
        }
        
        // Check for audio ports
        if (lilv_port_is_a(plugin, port, audio_class)) {
            if (lilv_port_is_a(plugin, port, input_class)) {
                if (audioInputPortL_ == -1) {
                    audioInputPortL_ = i;
                } else if (audioInputPortR_ == -1) {
                    audioInputPortR_ = i;
                }
            } else if (lilv_port_is_a(plugin, port, output_class)) {
                if (audioOutputPortL_ == -1) {
                    audioOutputPortL_ = i;
                } else if (audioOutputPortR_ == -1) {
                    audioOutputPortR_ = i;
                }
            }
        }
        
        // Check if this is a control input port
        if (lilv_port_is_a(plugin, port, control_class) &&
            lilv_port_is_a(plugin, port, input_class)) {
            
            LV2PluginParameter param;
            param.variable = nullptr;  // Initialize to null
            
            // Get port label (preferred over name for UI display)
            LilvNode* label_uri = lilv_new_uri(world, "http://www.w3.org/2000/01/rdf-schema#label");
            const LilvNodes* label_nodes = lilv_port_get_value(plugin, port, label_uri);
            if (label_nodes && lilv_nodes_size(label_nodes) > 0) {
                const LilvNode* first_label = lilv_nodes_get_first(label_nodes);
                param.name = lilv_node_as_string(first_label);
                lilv_nodes_free((LilvNodes*)label_nodes);
            } else {
                // Fallback to port name if no label
                LilvNode* name_node = lilv_port_get_name(plugin, port);
                param.name = lilv_node_as_string(name_node);
                lilv_node_free(name_node);
            }
            lilv_node_free(label_uri);
            
            // Get port group information
            // Just get the group URI for now - the full name lookup via lilv_world_get is problematic
            LilvNode* group_uri = lilv_new_uri(world, "http://lv2plug.in/ns/ext/port-groups#group");
            const LilvNodes* group_nodes = lilv_port_get_value(plugin, port, group_uri);
            std::string groupName = "";
            if (group_nodes && lilv_nodes_size(group_nodes) > 0) {
                const LilvNode* first_group = lilv_nodes_get_first(group_nodes);
                // Extract short name from URI (e.g., "mutated:braids" -> "braids")
                if (lilv_node_is_uri(first_group)) {
                    const char* group_uri_str = lilv_node_as_uri(first_group);
                    const char* hash = strrchr(group_uri_str, '#');
                    const char* colon = strrchr(group_uri_str, ':');
                    if (hash) {
                        groupName = hash + 1;
                    } else if (colon) {
                        groupName = colon + 1;
                    }
                }
                lilv_nodes_free((LilvNodes*)group_nodes);
            }
            lilv_node_free(group_uri);
            
            // Store group name in parameter
            param.groupName = groupName;
            
            // Get port range with proper defaults from TTL
            LilvNode* def_node = nullptr;
            LilvNode* min_node = nullptr;
            LilvNode* max_node = nullptr;
            lilv_port_get_range(plugin, port, &def_node, &min_node, &max_node);
            
            param.minValue = min_node ? lilv_node_as_float(min_node) : 0.0f;
            param.maxValue = max_node ? lilv_node_as_float(max_node) : 1.0f;
            
            // Read scale points for enumerated parameters using the proper lilv API
            LilvScalePoints* scale_points = lilv_port_get_scale_points(plugin, port);
            if (scale_points) {
                LILV_FOREACH(scale_points, sp_iter, scale_points) {
                    const LilvScalePoint* sp = lilv_scale_points_get(scale_points, sp_iter);
                    if (sp) {
                        const LilvNode* label_node = lilv_scale_point_get_label(sp);
                        const LilvNode* value_node = lilv_scale_point_get_value(sp);
                        if (label_node && value_node) {
                            LV2ScalePoint scale_pt;
                            scale_pt.value = lilv_node_as_float(value_node);
                            scale_pt.label = std::string(lilv_node_as_string(label_node));
                            param.scalePoints.push_back(scale_pt);
                        }
                    }
                }
                lilv_scale_points_free(scale_points);
                
                // Sort scale points by value for consistent ordering
                if (!param.scalePoints.empty()) {
                    std::sort(param.scalePoints.begin(), param.scalePoints.end(),
                        [](const LV2ScalePoint& a, const LV2ScalePoint& b) {
                            return a.value < b.value;
                        });
                }
            }
            
            // Also check for enumeration property which indicates discrete values
            LilvNode* port_property_uri = lilv_new_uri(world, "http://lv2plug.in/ns/lv2core#portProperty");
            const LilvNodes* port_properties = lilv_port_get_value(plugin, port, port_property_uri);
            bool isEnumeration = false;
            if (port_properties) {
                LILV_FOREACH(nodes, prop_iter, port_properties) {
                    const LilvNode* property = lilv_nodes_get(port_properties, prop_iter);
                    const char* prop_uri = lilv_node_as_uri(property);
                    if (strcmp(prop_uri, "http://lv2plug.in/ns/lv2core#enumeration") == 0) {
                        isEnumeration = true;
                        break;
                    }
                }
                lilv_nodes_free((LilvNodes*)port_properties);
            }
            lilv_node_free(port_property_uri);

            // Store enumeration flag so we can handle discrete parameters later
            param.isEnumeration = isEnumeration;
            
            // Use TTL default value if available, otherwise use minimum
            if (def_node) {
                param.defaultValue = lilv_node_as_float(def_node);
            } else {
                // If no default specified, use a sensible default based on range

                if (param.minValue >= 0.0f && param.maxValue <= 1.0f) {
                    // Normalized range, default to 0.5
                    param.defaultValue = 0.5f;
                } else if (param.minValue < 0.0f && param.maxValue > 0.0f) {
                    // Bipolar range, default to 0
                    param.defaultValue = 0.0f;
                } else {
                    // Use minimum value as default
                    param.defaultValue = param.minValue;
                }
            }
            
            param.currentValue = param.defaultValue;
            param.portIndex = i;
            
            // Create a Variable with better naming
            // Use group prefix if available, otherwise use parameter label
            std::string varDisplayName = param.name;
            if (!groupName.empty()) {
                varDisplayName = groupName + ":" + param.name;
            }
            
            // Limit variable name length and create unique variable name
            char varName[64];
            if (varDisplayName.length() > 20) {
                // Truncate long names but keep meaningful part
                std::string shortName = varDisplayName.substr(0, 17) + "...";
                snprintf(varName, 64, "%s", shortName.c_str());
            } else {
                snprintf(varName, 64, "%s", varDisplayName.c_str());
            }
            
            // Scale default value to 0-127 for UI (use rounding for better precision)
            int scaledValue = (int)(((param.currentValue - param.minValue) / (param.maxValue - param.minValue) * 127.0f) + 0.5f);
            if (scaledValue < 0) scaledValue = 0;
            if (scaledValue > 127) scaledValue = 127;
            
            param.variable = new Variable(varName, MAKE_FOURCC('L','P',i/256,i%256), scaledValue);
            Insert(param.variable);
            
            lilv_node_free(def_node);
            lilv_node_free(min_node);
            lilv_node_free(max_node);
            
            parameters_.push_back(param);
            
            // Allocate storage for this control value
            controlValues_.push_back(param.defaultValue);

            // Diagnostic dump for braids/plaits to inspect scale points and ranges (helps debug crashes)
            if (param.groupName == "braids" || param.groupName == "plaits") {
                Trace::Log("LV2","Param discovered: %s:%s port=%d min=%f max=%f default=%f enum=%d scalePoints=%zu",
                    param.groupName.c_str(), param.name.c_str(), param.portIndex, param.minValue, param.maxValue, param.defaultValue, param.isEnumeration ? 1 : 0, param.scalePoints.size());
                for (size_t sp=0; sp<param.scalePoints.size(); ++sp) {
                    Trace::Log("LV2","  scalepoint[%zu] = %f -> '%s'", sp, param.scalePoints[sp].value, param.scalePoints[sp].label.c_str());
                }
            }
        }
    }
    
    lilv_node_free(atom_port);
    lilv_node_free(midi_event);
    lilv_node_free(audio_class);
    lilv_node_free(control_class);
    lilv_node_free(output_class);
    lilv_node_free(input_class);
    
    // Allocate MIDI buffer if we have a MIDI port (16KB should be enough)
    if (midiInputPort_ >= 0 && !midiBuffer_) {
        midiBufferSize_ = 16384;
        midiBuffer_ = new uint8_t[midiBufferSize_];
    }
}

void LV2Instrument::connectPorts(int bufferSize) {
    if (!pluginInstance_) {
        return;
    }
    
    LilvInstance* instance = (LilvInstance*)pluginInstance_;
    
    // Connect audio input buffers
    if (audioInputPortL_ >= 0 && audioInputL_) {
        lilv_instance_connect_port(instance, audioInputPortL_, audioInputL_);
    }
    if (audioInputPortR_ >= 0 && audioInputR_) {
        lilv_instance_connect_port(instance, audioInputPortR_, audioInputR_);
    }
    
    // Connect audio output buffers
    if (audioOutputPortL_ >= 0 && audioBufferL_) {
        lilv_instance_connect_port(instance, audioOutputPortL_, audioBufferL_);
    }
    if (audioOutputPortR_ >= 0 && audioBufferR_) {
        lilv_instance_connect_port(instance, audioOutputPortR_, audioBufferR_);
    }
    
    // Connect MIDI input port if available
    if (midiInputPort_ >= 0 && midiBuffer_) {
        // Initialize an empty atom sequence
        typedef struct {
            uint32_t atom_size;
            uint32_t atom_type;
            uint32_t body_size;
            uint32_t body_pad;
        } LV2_Atom_Sequence_Header;
        
        LV2_Atom_Sequence_Header* seq = (LV2_Atom_Sequence_Header*)midiBuffer_;
        seq->atom_size = sizeof(uint32_t) * 2; // Just header, no events initially
        seq->atom_type = 0; // Will be set properly with URIDs
        seq->body_size = 0;
        seq->body_pad = 0;
        
        lilv_instance_connect_port(instance, midiInputPort_, midiBuffer_);
    }
    
    // If mono output, connect same buffer to both channels
    if (audioOutputPortL_ >= 0 && audioOutputPortR_ == -1 && audioBufferR_) {
        lilv_instance_connect_port(instance, audioOutputPortL_, audioBufferR_);
    }
    
    // Connect control parameters
    // Ensure we have a stable separate storage indexed by port index so plugin pointers are unique and immutable
    int maxPort = -1;
    for (size_t i = 0; i < parameters_.size(); ++i) {
        if (parameters_[i].portIndex > maxPort) maxPort = parameters_[i].portIndex;
    }
    if (maxPort >= 0) {
        // Resize storage if necessary (keep previous values if possible)
        if ((int)portControlStorage_.size() <= maxPort) portControlStorage_.resize(maxPort + 1, 0.0f);
    }

    for (size_t i = 0; i < parameters_.size() && i < controlValues_.size(); i++) {
        // Update both controlValues_ (indexed by parameter order) and per-port storage (indexed by port index)
        controlValues_[i] = parameters_[i].currentValue;
        if (parameters_[i].portIndex >= 0) {
            if ((int)portControlStorage_.size() <= parameters_[i].portIndex) {
                // Safety: expand if needed (shouldn't normally happen)
                portControlStorage_.resize(parameters_[i].portIndex + 1, 0.0f);
            }
            portControlStorage_[parameters_[i].portIndex] = parameters_[i].currentValue;

            // Diagnostic logging for braids parameters and for unexpected large port indexes
            if (parameters_[i].groupName == "braids") {
                Trace::Log("LV2","Connect braids param '%s' to port=%d addr=%p value=%f",
                    parameters_[i].name.c_str(), parameters_[i].portIndex, (void*)&portControlStorage_[parameters_[i].portIndex], portControlStorage_[parameters_[i].portIndex]);
            }

            // Avoid connecting to obviously invalid ports (negative) and log suspicious indexes
            if (parameters_[i].portIndex < 0 || parameters_[i].portIndex > 65536) {
                Trace::Error("LV2","Suspicious port index %d for param %s", parameters_[i].portIndex, parameters_[i].name.c_str());
            }

            lilv_instance_connect_port(instance, parameters_[i].portIndex, &portControlStorage_[parameters_[i].portIndex]);
        } else {
            Trace::Log("LV2","Skipping connect for param %s with invalid portIndex=%d", parameters_[i].name.c_str(), parameters_[i].portIndex);
        }
    }
}

void LV2Instrument::SetParameterValue(int index, float value) {
    if (index >= 0 && index < (int)parameters_.size()) {
        parameters_[index].currentValue = value;
        if (index < (int)controlValues_.size()) {
            controlValues_[index] = value;
        }
        // Also sync to port indexed storage so connected plugin ports receive the new value
        int pidx = parameters_[index].portIndex;
        if (pidx >= 0) {
            if ((int)portControlStorage_.size() <= pidx) {
                portControlStorage_.resize(pidx + 1, 0.0f);
            }
            portControlStorage_[pidx] = value;
        }
    }
}

std::string LV2Instrument::GetParameterScalePointLabel(int paramIndex, float value) const {
    if (paramIndex < 0 || paramIndex >= (int)parameters_.size()) {
        return "";
    }
    
    const LV2PluginParameter& param = parameters_[paramIndex];
    
    // Find the closest scale point to the given value
    if (!param.scalePoints.empty()) {
        float minDiff = std::abs(param.scalePoints[0].value - value);
        int bestIndex = 0;
        
        for (size_t i = 1; i < param.scalePoints.size(); i++) {
            float diff = std::abs(param.scalePoints[i].value - value);
            if (diff < minDiff) {
                minDiff = diff;
                bestIndex = i;
            }
        }
        
        // Return the closest scale point label
        return param.scalePoints[bestIndex].label;
    }
    
    return ""; // No scale points defined
}

#include "LV2Instrument.h"
#include "CommandList.h"
#include "System/Console/Trace.h"
#include <string.h>
#include <cmath>
#include <map>
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
    }

    // Setup variables
    Variable *v = new Variable("plugin", LV2IP_PLUGIN, -1);
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
    // TODO: Initialize LV2 plugin here when plugin hosting is implemented
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

    Trace::Log("LV2", "Note on: channel=%d note=%d (queued)", channel, note);

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

    Trace::Log("LV2", "Note off: channel=%d note=%d (queued)", channel, lastNote_[channel]);
}

bool LV2Instrument::Render(int channel, fixed *buffer, int size, bool updateTick) {
    static int renderCallCount = 0;
    if (renderCallCount++ % 100 == 0) {
        Trace::Log("LV2", "Render called: channel=%d size=%d playing=%d empty=%d", 
                   channel, size, playing_[channel], IsEmpty());
    }
    
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
        Trace::Log("LV2", "Allocating audio buffers size=%d", BUFFER_SIZE);
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
        Trace::Log("LV2", "Plugin activated");
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
            controlValues_[i] = realValue;
            parameters_[i].currentValue = realValue;
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
            
            static int logCount = 0;
            if (logCount++ < 10) {
                Trace::Log("LV2", "Wrote MIDI event: %02X %02X %02X (URID=%d)", 
                           evt.data[0], evt.data[1], evt.data[2], g_midiEventUrid);
            }
        }
        pendingMidiEvents_.clear();
        
        // Update final sequence size
        seq->atom.size = sizeof(LV2_Atom_Sequence_Body) + offset;
    }

    // Run the plugin to generate output
    LilvInstance* instance = (LilvInstance*)pluginInstance_;
    lilv_instance_run(instance, size);

    // Convert float output to fixed point stereo interleaved
    // Apply volume control
    Variable *volVar = FindVariable(LV2IP_VOLUME);
    float volume = volVar ? (volVar->GetInt() / 255.0f) : 1.0f;

    // Check if we're getting any audio
    float maxSample = 0.0f;
    for (int i = 0; i < size; i++) {
        if (fabs(audioBufferL_[i]) > maxSample) maxSample = fabs(audioBufferL_[i]);
        if (fabs(audioBufferR_[i]) > maxSample) maxSample = fabs(audioBufferR_[i]);
    }
    
    // DEBUG: If no audio from plugin, generate test tone
    if (maxSample < 0.001f) {
        static int logCount = 0;
        if (logCount++ % 100 == 0) {
            Trace::Log("LV2", "No audio from plugin, generating test tone");
        }
        
        // Generate simple sine wave test tone at 440Hz
        static float phase = 0.0f;
        float frequency = 440.0f;
        float sampleRate = 44100.0f;
        float phaseIncrement = (frequency * 2.0f * 3.14159f) / sampleRate;
        
        for (int i = 0; i < size; i++) {
            float sample = sinf(phase) * 0.3f; // 30% amplitude
            audioBufferL_[i] = sample;
            audioBufferR_[i] = sample;
            phase += phaseIncrement;
            if (phase > 6.28318f) phase -= 6.28318f;
        }
    } else {
        static int logCount = 0;
        if (logCount++ % 100 == 0) {
            Trace::Log("LV2", "Audio output: max=%.3f", maxSample);
        }
    }

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

    static int debugCount = 0;
    if (debugCount++ % 100 == 0) {
        Trace::Log("LV2", "Render returning: vol=%.2f, buf[0]=%d, buf[1]=%d", 
                   volume, buffer[0], buffer[1]);
    }

    return true;
}

void LV2Instrument::ProcessCommand(int channel, FourCC cc, ushort value) {
    // TODO: Handle commands like volume, pan, etc.
    // Could be mapped to LV2 plugin parameters
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
    // Delete parameter variables before clearing
    for (size_t i = 0; i < parameters_.size(); i++) {
        if (parameters_[i].variable) {
            delete parameters_[i].variable;
            parameters_[i].variable = nullptr;
        }
    }
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
    }
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
        Trace::Log("LV2", "URIDs initialized: midiEvent=%d, atomSequence=%d", g_midiEventUrid, g_atomSequenceUrid);
    }
    
    // Instantiate the plugin with 44100 Hz sample rate and URID features
    LilvInstance* instance = lilv_plugin_instantiate(plugin, 44100.0, g_features);
    if (!instance) {
        Trace::Error("LV2: Failed to instantiate plugin: %s", pluginURI_);
        cleanupPlugin();
        return;
    }
    
    pluginInstance_ = instance;
    Trace::Log("LV2", "Successfully loaded plugin: %s", name_);
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
            const LilvNodes* buf_types = lilv_port_get_value(plugin, port,
                lilv_new_uri(world, "http://lv2plug.in/ns/ext/atom#bufferType"));
            if (buf_types) {
                midiInputPort_ = i;
                lilv_nodes_free((LilvNodes*)buf_types);
                Trace::Log("LV2", "Found MIDI atom input port at index %d", i);
            }
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
            
            // Get port name
            LilvNode* name_node = lilv_port_get_name(plugin, port);
            param.name = lilv_node_as_string(name_node);
            lilv_node_free(name_node);
            
            // Get port range (min, max, default)
            LilvNode* def_node = nullptr;
            LilvNode* min_node = nullptr;
            LilvNode* max_node = nullptr;
            lilv_port_get_range(plugin, port, &def_node, &min_node, &max_node);
            
            param.minValue = min_node ? lilv_node_as_float(min_node) : 0.0f;
            param.maxValue = max_node ? lilv_node_as_float(max_node) : 1.0f;
            param.defaultValue = def_node ? lilv_node_as_float(def_node) : param.minValue;
            param.currentValue = param.defaultValue;
            param.portIndex = i;
            
            // Create a Variable for this parameter (scaled to 0-127 for UI)
            int scaledValue = (int)((param.currentValue - param.minValue) / (param.maxValue - param.minValue) * 127.0f);
            if (scaledValue < 0) scaledValue = 0;
            if (scaledValue > 127) scaledValue = 127;
            
            char varName[64];
            snprintf(varName, 64, "p%d", (int)parameters_.size());
            param.variable = new Variable(varName, MAKE_FOURCC('L','P',i/256,i%256), scaledValue);
            Insert(param.variable);
            
            lilv_node_free(def_node);
            lilv_node_free(min_node);
            lilv_node_free(max_node);
            
            parameters_.push_back(param);
            
            // Allocate storage for this control value
            controlValues_.push_back(param.defaultValue);
        }
    }
    
    lilv_node_free(atom_port);
    lilv_node_free(midi_event);
    lilv_node_free(audio_class);
    lilv_node_free(control_class);
    lilv_node_free(output_class);
    lilv_node_free(input_class);
    
    Trace::Log("LV2", "Discovered %d control parameters, audio ports: in=%d/%d out=%d/%d, MIDI: %d", 
               (int)parameters_.size(), audioInputPortL_, audioInputPortR_, audioOutputPortL_, audioOutputPortR_, midiInputPort_);
    
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
        Trace::Log("LV2", "Connected MIDI port %d", midiInputPort_);
    }
    
    // If mono output, connect same buffer to both channels
    if (audioOutputPortL_ >= 0 && audioOutputPortR_ == -1 && audioBufferR_) {
        lilv_instance_connect_port(instance, audioOutputPortL_, audioBufferR_);
    }
    
    // Connect control parameters
    for (size_t i = 0; i < parameters_.size() && i < controlValues_.size(); i++) {
        controlValues_[i] = parameters_[i].currentValue;
        lilv_instance_connect_port(instance, parameters_[i].portIndex, &controlValues_[i]);
    }
    
    Trace::Log("LV2", "Connected ports");
}

void LV2Instrument::SetParameterValue(int index, float value) {
    if (index >= 0 && index < (int)parameters_.size()) {
        parameters_[index].currentValue = value;
        if (index < (int)controlValues_.size()) {
            controlValues_[index] = value;
        }
    }
}

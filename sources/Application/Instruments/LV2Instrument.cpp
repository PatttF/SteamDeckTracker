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
#include <lv2/atom/forge.h>
#include <lv2/patch/patch.h>
#ifndef LV2_RDF__type
#define LV2_RDF__type "http://www.w3.org/1999/02/22-rdf-syntax-ns#type"
#endif
#include <lv2/midi/midi.h>
#include <lv2/options/options.h>
#include <lv2/buf-size/buf-size.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fstream>

#include "Foundation/Variables/WatchedVariable.h"

// LV2 URI definitions
#undef LV2_ATOM__Sequence
#undef LV2_MIDI__MidiEvent
#define LV2_ATOM__Sequence "http://lv2plug.in/ns/ext/atom#Sequence"
#define LV2_MIDI__MidiEvent "http://lv2plug.in/ns/ext/midi#MidiEvent"

// Global URID map for this instance
static std::map<std::string, LV2_URID> g_uridMap;
static LV2_URID g_nextUrid = 1;

LV2_URID urid_map(LV2_URID_Map_Handle handle, const char* uri) {
    auto it = g_uridMap.find(uri);
    if (it != g_uridMap.end()) {
        return it->second;
    }
    LV2_URID urid = g_nextUrid++;
    g_uridMap[uri] = urid;
    return urid;
}

const char* urid_unmap(LV2_URID_Unmap_Handle handle, LV2_URID urid) {
    for (auto& pair : g_uridMap) {
        if (pair.second == urid) {
            return pair.first.c_str();
        }
    }
    return nullptr;
}

// LV2 features
LV2_URID_Map g_uridMapFeature = { nullptr, urid_map };
LV2_URID_Unmap g_uridUnmapFeature = { nullptr, urid_unmap };

LV2_Feature g_mapFeature = { LV2_URID__map, &g_uridMapFeature };
LV2_Feature g_unmapFeature = { LV2_URID__unmap, &g_uridUnmapFeature };

// Minimal options array (zero-terminated) for LV2_OPTIONS__options feature
// We'll provide explicit entries for min, nominal and max block lengths plus a zero terminator.
static uint32_t s_minBlock = 64u;
static uint32_t s_nominalBlock = 1024u;
static uint32_t s_maxBlock = 131072u;
LV2_Options_Option g_optionsArray[4] = {
    { (LV2_Options_Context)0, 0u, 0u, 0u, 0u, nullptr }, // max block length (filled at runtime)
    { (LV2_Options_Context)0, 0u, 0u, 0u, 0u, nullptr }, // nominal block length (filled at runtime)
    { (LV2_Options_Context)0, 0u, 0u, 0u, 0u, nullptr }, // min block length (filled at runtime)
    { (LV2_Options_Context)0, 0u, 0u, 0u, 0u, nullptr }  // zero terminator
};
LV2_Feature g_optionsFeature = { LV2_OPTIONS__options, (void*)g_optionsArray };

// LV2 Options interface implementation to respond to queries such as
// "http://lv2plug.in/ns/ext/buf-size#maxBlockLength" made by plugins.
static uint32_t lv2_options_get(LV2_Handle instance, LV2_Options_Option* options)
{
    if (!options) return LV2_OPTIONS_ERR_UNKNOWN;

    uint32_t status = LV2_OPTIONS_SUCCESS;

    for (LV2_Options_Option* opt = options; opt && (opt->context || opt->subject || opt->key || opt->size || opt->type || opt->value); ++opt) {
        // Keys are URIDs; map known keys to values
        if (opt->key == urid_map(nullptr, LV2_BUF_SIZE__maxBlockLength)) {
            opt->size = sizeof(uint32_t);
            opt->type = urid_map(nullptr, LV2_ATOM__Int);
            opt->value = &s_maxBlock;
        } else if (opt->key == urid_map(nullptr, LV2_BUF_SIZE__minBlockLength)) {
            opt->size = sizeof(uint32_t);
            opt->type = urid_map(nullptr, LV2_ATOM__Int);
            opt->value = &s_minBlock;
        } else if (opt->key == urid_map(nullptr, LV2_BUF_SIZE__nominalBlockLength)) {
            opt->size = sizeof(uint32_t);
            opt->type = urid_map(nullptr, LV2_ATOM__Int);
            opt->value = &s_nominalBlock;
        } else {
            // Unknown key
            status |= LV2_OPTIONS_ERR_BAD_KEY;
        }
    }

    return status;
}

static uint32_t lv2_options_set(LV2_Handle instance, const LV2_Options_Option* options)
{
    // We don't support setting options at runtime; return BAD_KEY for unhandled
    (void)instance; (void)options;
    return LV2_OPTIONS_ERR_BAD_KEY;
}

LV2_Options_Interface g_optionsInterface = { lv2_options_get, lv2_options_set };
LV2_Feature g_optionsInterfaceFeature = { LV2_OPTIONS__interface, &g_optionsInterface };

// Fill the options array keys and types using the URID map. Call this before instantiating any plugin.
static void init_options_array()
{
    g_optionsArray[0].key   = urid_map(nullptr, LV2_BUF_SIZE__maxBlockLength);
    g_optionsArray[0].size  = sizeof(uint32_t);
    g_optionsArray[0].type  = urid_map(nullptr, LV2_ATOM__Int);
    g_optionsArray[0].value = &s_maxBlock;

    g_optionsArray[1].key   = urid_map(nullptr, LV2_BUF_SIZE__nominalBlockLength);
    g_optionsArray[1].size  = sizeof(uint32_t);
    g_optionsArray[1].type  = urid_map(nullptr, LV2_ATOM__Int);
    g_optionsArray[1].value = &s_nominalBlock;

    g_optionsArray[2].key   = urid_map(nullptr, LV2_BUF_SIZE__minBlockLength);
    g_optionsArray[2].size  = sizeof(uint32_t);
    g_optionsArray[2].type  = urid_map(nullptr, LV2_ATOM__Int);
    g_optionsArray[2].value = &s_minBlock;

    // g_optionsArray[3] remains the zero terminator
}
// Minimal bounded block length data for LV2_BUF_SIZE__boundedBlockLength feature
static uint32_t g_boundedBlockLength[2] = {64, 131072};
LV2_Feature g_boundedFeature = { LV2_BUF_SIZE__boundedBlockLength, &g_boundedBlockLength };

static const LV2_Feature* g_features[] = { &g_mapFeature, &g_unmapFeature, &g_optionsFeature, &g_optionsInterfaceFeature, &g_boundedFeature, nullptr };

// Cached URIDs for performance
LV2_URID g_midiEventUrid = 0;
LV2_URID g_atomSequenceUrid = 0;

LV2_URID g_atomFloatUrid = 0;

LV2Instrument::LV2Instrument() {
    strcpy(name_, "LV2");
    pluginURI_[0] = '\0';
    world_ = static_cast<LilvWorld*>(lilv_world_new());
    plugin_ = nullptr; // Ensure plugin_ is compatible with LilvPlugin*
    pluginInstance_ = static_cast<LilvInstance*>(nullptr);
    audioBufferL_ = nullptr;
    audioBufferR_ = nullptr;
    audioInputL_ = nullptr;
    audioInputR_ = nullptr;
    audioDummyBuffer_ = nullptr;
    bufferSize_ = 0;
    audioInputPortL_ = -1;
    audioInputPortR_ = -1;
    audioOutputPortL_ = -1;
    audioOutputPortR_ = -1;
    midiInputPort_ = -1;
    midiBuffer_ = nullptr;
    midiBufferSize_ = 0;
    isActivated_ = false;
    forcedOutputChannels_ = 0; // default to automatic

    // Initialize options array entries so plugins can read block-size options
    init_options_array();

    // Initialize channel state
    for (int i = 0; i < SONG_CHANNEL_COUNT; i++) {
        lastNote_[i] = -1;
        playing_[i] = false;
        // Initialize reverb state
        memset(reverbBuffer_[i], 0, sizeof(reverbBuffer_[i]));
        reverbDecay_[i] = 0;
        reverbSend_[i] = 0;
        reverbPos_[i] = 0; // Initialize reverb state
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

    Trace::Debug("LV2Instrument: Start() queued note-on ch=%d note=%d vel=100 pending=%zu", channel, (int)note, pendingMidiEvents_.size());

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
        audioDummyBuffer_ = new float[BUFFER_SIZE];
        bufferSize_ = BUFFER_SIZE;
        connectPorts(BUFFER_SIZE);
        // Pre-allocate pending atom events to avoid realtime allocations during fast param changes
        pendingAtomEvents_.reserve(128);
    }

    // Activate plugin once after first connection
    if (!isActivated_ && pluginInstance_) {
        LilvInstance* instance = (LilvInstance*)pluginInstance_;
        lilv_instance_activate(instance);
        isActivated_ = true;

        // Send default values for control ports to the now-active plugin.
        // Control ports: write defaults into portControlStorage_ (the plugin
        // reads these directly).
        // NOTE: Do NOT send patch:Set atom messages during activation — sending
        // hundreds of patch:Set defaults floods the atom buffer and can prevent
        // the plugin from processing MIDI note-on events on the first render.
        // The plugin's own instantiation should have set up sensible defaults.
        for (size_t pi = 0; pi < parameters_.size(); ++pi) {
            int pidx = parameters_[pi].portIndex;
            if (pidx >= 0 && !parameters_[pi].isAtomPort) {
                // Direct control port: ensure storage has the default
                if (pidx < (int)portControlStorage_.size()) {
                    portControlStorage_[pidx] = parameters_[pi].currentValue;
                }
            }
        }
        Trace::Debug("LV2Instrument: Activated plugin and sent %zu control port defaults", parameters_.size());
    }

    // Clear input buffers (no audio input for synths)
    for (int i = 0; i < size; i++) {
        audioInputL_[i] = 0.0f;
        audioInputR_[i] = 0.0f;
    }

    // Update parameter values from Variables. If values changed we call SetParameterValue so that
    // both direct control ports and resource-backed parameters (patch:Set) are handled consistently.
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
                realValue = closest;
            } else if (parameters_[i].isEnumeration) {
                // No explicit labels available, but port is enumerated: round to integer within range
                float rounded = std::round(realValue);
                if (rounded < parameters_[i].minValue) rounded = parameters_[i].minValue;
                if (rounded > parameters_[i].maxValue) rounded = parameters_[i].maxValue;
                realValue = rounded;
            }

            // If value changed, propagate it using SetParameterValue which will update port storage
            // or queue a patch:Set for resource-backed parameters.
            if (std::fabs(realValue - parameters_[i].currentValue) > 1e-6f) {
                // Update controlValues_ for local storage and call the centralized setter
                controlValues_[i] = realValue;
                Trace::Debug("LV2Instrument: Update - calling SetParameterValue paramIndex=%d portIndex=%d isAtom=%d name=%s",
                             (int)i, parameters_[i].portIndex, parameters_[i].isAtomPort ? 1 : 0, parameters_[i].name.c_str());
                SetParameterValue((int)i, realValue);
            } else {
                // Keep controlValues_ in sync even if unchanged
                controlValues_[i] = parameters_[i].currentValue;
            }
        }
    }

    // Write pending MIDI events and atom messages to the atom buffer
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


        
        // Write pending MIDI events (as before)
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
            
            Trace::Debug("LV2Instrument: Wrote MIDI evt[%zu] status=0x%02X data1=%d data2=%d to midiBuffer_ offset=%zu midiEventUrid=%u",
                i, evt.data[0], evt.data[1], evt.data[2], offset, g_midiEventUrid);
        }
        pendingMidiEvents_.clear();

        // Commit sequence size after MIDI events so appendEventsToBuffer knows where to append
        seq->atom.size = sizeof(LV2_Atom_Sequence_Body) + offset;

        // Write pending atom events (grouped by destination port)
        if (!pendingAtomEvents_.empty()) {
            // Group events by dest port
            std::map<int, std::vector<PendingAtomEvent>> groups;
            for (auto &ae : pendingAtomEvents_) {
                auto &vec = groups[ae.destPortIndex];
                if (vec.empty()) vec.reserve(4); // small reserve to avoid frequent reallocations under bursts
                vec.push_back(ae);
            }

            // Helper to append events to an existing atom sequence buffer (preserves any prior events)
            auto appendEventsToBuffer = [&](uint8_t* buffer, size_t bufCap, const std::vector<PendingAtomEvent> &events) {
                if (!buffer) return;
                LV2_Atom_Sequence* seq = (LV2_Atom_Sequence*)buffer;

                // If buffer is not yet initialised as a sequence, initialise it now
                LV2_URID seqType = g_atomSequenceUrid ? g_atomSequenceUrid : urid_map(nullptr, LV2_ATOM__Sequence);
                if (seq->atom.type != seqType) {
                    seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
                    seq->atom.type = seqType;
                    seq->body.unit = 0;
                    seq->body.pad = 0;
                }

                // Compute where existing data ends so we append after it
                size_t existingDataSize = seq->atom.size - sizeof(LV2_Atom_Sequence_Body);
                uint8_t* b = (uint8_t*)LV2_ATOM_CONTENTS(LV2_Atom_Sequence, seq);
                size_t off = existingDataSize;
                size_t cap = bufCap - sizeof(LV2_Atom_Sequence);

                for (const auto &e : events) {
                    size_t padded_size = sizeof(LV2_Atom_Event) + ((e.data.size() + 7) & ~7);
                    if (off + padded_size > cap) break;
                    LV2_Atom_Event* event = (LV2_Atom_Event*)(b + off);
                    event->time.frames = 0;
                    event->body.size = (uint32_t)e.data.size();
                    event->body.type = e.type;
                    memcpy(LV2_ATOM_BODY(&event->body), e.data.data(), e.data.size());
                    off += padded_size;
                }
                seq->atom.size = sizeof(LV2_Atom_Sequence_Body) + off;
            };

            // Append to MIDI/primary atom input if present (preserves MIDI events already in buffer)
            auto it = groups.find(midiInputPort_);
            if (it != groups.end() && midiBuffer_) {
                appendEventsToBuffer(midiBuffer_, midiBufferSize_, it->second);
                Trace::Debug("LV2Instrument: appended %u atom events to midiInputPort=%d", (unsigned)it->second.size(), midiInputPort_);
            }

            // Append to other atom input ports
            for (auto &g : groups) {
                int dest = g.first;
                if (dest == midiInputPort_) continue;
                if (dest >= 0 && dest < (int)atomInputBuffers_.size() && atomInputBuffers_[dest]) {
                    appendEventsToBuffer(atomInputBuffers_[dest], atomInputBufferSizes_[dest], g.second);
                    Trace::Debug("LV2Instrument: appended %u atom events to atom input port=%d", (unsigned)g.second.size(), dest);
                } else {
                    Trace::Debug("LV2Instrument: dropping %u atom events for unconnected port=%d", (unsigned)g.second.size(), dest);
                }
            }

            // Broadcast fallback: some events may have been queued with destPortIndex==-1 (no midi input),
            // or plugins may listen on a different atom input port. As a best-effort fallback, write events
            // with negative destination to all available atom input ports (and midi buffer if present).
            std::vector<PendingAtomEvent> broadcastEvents;
            auto itneg = groups.find(-1);
            if (itneg != groups.end()) {
                broadcastEvents = itneg->second;
            } else {
                for (auto &g : groups) {
                    if (g.first < 0) {
                        broadcastEvents.insert(broadcastEvents.end(), g.second.begin(), g.second.end());
                    }
                }
            }

            if (!broadcastEvents.empty()) {
                if (midiBuffer_) {
                    appendEventsToBuffer(midiBuffer_, midiBufferSize_, broadcastEvents);
                }
                for (int port = 0; port < (int)atomInputBuffers_.size(); ++port) {
                    if (atomInputBuffers_[port]) {
                        appendEventsToBuffer(atomInputBuffers_[port], atomInputBufferSizes_[port], broadcastEvents);
                    }
                }
            }
        }
        pendingAtomEvents_.clear();
    }

    // Re-initialize atom OUTPUT buffers as empty sequences before each run,
    // so the plugin has space to write notification/output atoms.
    for (size_t oi = 0; oi < atomOutputBuffers_.size(); ++oi) {
        if (atomOutputBuffers_[oi]) {
            LV2_Atom_Sequence* seq = (LV2_Atom_Sequence*)atomOutputBuffers_[oi];
            seq->atom.type = g_atomSequenceUrid;
            seq->atom.size = atomOutputBufferSizes_[oi] - sizeof(LV2_Atom);
            seq->body.unit = 0;
            seq->body.pad = 0;
        }
    }

    // Run the plugin to generate output
    LilvInstance* instance = (LilvInstance*)pluginInstance_;

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
            int fb_pos = reverbPos - LV2_REVERB_BUFFER_LENGTH + 100;
            if (fb_pos < 0) fb_pos += LV2_REVERB_BUFFER_LENGTH;
            fixed fbL = fp_mul(reverbBuf[fb_pos * 2], decay);
            fixed fbR = fp_mul(reverbBuf[fb_pos * 2 + 1], decay);

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
        
        Trace::Debug("REVERB: LV2 Channel %d: decay=%d send=%d", channel, decayVal, sendAmount);
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
    // Remove observers we added to any WatchedVariables
    for (size_t i=0;i<parameters_.size();++i) {
        if (parameters_[i].variable) {
            WatchedVariable *wv = dynamic_cast<WatchedVariable*>(parameters_[i].variable);
            if (wv) wv->RemoveObserver(*this);
        }
    }
    cleanupPlugin();
    pluginURI_[0] = '\0';
    strcpy(name_, "LV2");
    parameters_.clear();
    controlValues_.clear();
}

void LV2Instrument::Update(Observable &o, I_ObservableData *d) {
    // Called when a WatchedVariable changes (user edited a parameter in the UI)
    Variable *v = dynamic_cast<Variable*>(&o);
    if (!v) return;

    // Find which parameter this variable corresponds to
    for (size_t i = 0; i < parameters_.size(); ++i) {
        if (parameters_[i].variable == v) {
            int scaled = v->GetInt();
            float realValue = parameters_[i].minValue + (scaled / 127.0f) * (parameters_[i].maxValue - parameters_[i].minValue);
            Trace::Debug("LV2Instrument: Update - variable changed, paramIndex=%zu scaled=%d real=%g", i, scaled, realValue);
            SetParameterValue((int)i, realValue);
            break;
        }
    }
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
    if (audioDummyBuffer_) {
        delete[] audioDummyBuffer_;
        audioDummyBuffer_ = nullptr;
    }
    if (midiBuffer_) {
        delete[] midiBuffer_;
        midiBuffer_ = nullptr;
        midiBufferSize_ = 0;
    }

    // Free any allocated atom output buffers
    for (size_t i = 0; i < atomOutputBuffers_.size(); ++i) {
        if (atomOutputBuffers_[i]) {
            delete[] atomOutputBuffers_[i];
        }
    }
    atomOutputBuffers_.clear();
    atomOutputBufferSizes_.clear();

    // Free allocated atom input buffers
    for (size_t i = 0; i < atomInputBuffers_.size(); ++i) {
        if (atomInputBuffers_[i]) {
            delete[] atomInputBuffers_[i];
        }
    }
    atomInputBuffers_.clear();
    atomInputBufferSizes_.clear();

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
        
        // Name will be resolved from the plugin manifest if available; set a quick placeholder first
        strncpy(name_, "LV2", 79); name_[79] = '\0';
        Trace::Debug("LV2Instrument: Setting plugin URI=%s (name will be resolved)", pluginURI_);
        
        // Load the plugin and discover parameters
        loadPlugin();
        discoverParameters();

        // Try to obtain a human-friendly name from the loaded plugin metadata (doap:name / rdfs:label)
        if (world_ && plugin_) {
            const LilvPlugin* lplug = (const LilvPlugin*)plugin_;
            LilvNode* name_node = lilv_plugin_get_name(lplug);
            if (name_node) {
                const char* n = lilv_node_as_string(name_node);
                if (n && n[0]) {
                    strncpy(name_, n, 79);
                    name_[79] = '\0';
                    Trace::Debug("LV2Instrument: Resolved plugin name: %s", name_);
                }
                lilv_node_free(name_node);
            }
        }

        // NOTE: We intentionally do NOT send patch:Set atom messages for
        // resource-backed parameters here. The plugin's own instantiation sets
        // up sensible defaults, and flooding the atom buffer with hundreds of
        // patch:Set messages before the first render can prevent the plugin
        // from processing MIDI events.
        Trace::Debug("LV2Instrument: Plugin loaded with %zu parameters (defaults from plugin's own init)", parameters_.size());


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
        return;
    }
    
    LilvWorld* world = static_cast<LilvWorld*>(world_);
    lilv_world_load_all(world);
    
    // Find the plugin by URI
    LilvNode* uri_node = lilv_new_uri(static_cast<LilvWorld*>(world_), pluginURI_);
    if (!uri_node) {
        cleanupPlugin();
        return;
    }
    
    const LilvPlugin* plugin = lilv_plugins_get_by_uri(lilv_world_get_all_plugins(world), uri_node);
    lilv_node_free(uri_node);
    
    if (!plugin) {
        Trace::Error("LV2Instrument: Plugin not found: %s", pluginURI_);
        cleanupPlugin();
        return;
    }
    
    plugin_ = (void*)plugin;
    Trace::Debug("LV2Instrument: Found plugin: %s", pluginURI_);

    // Load the plugin's bundle and resource data so all triples (including
    // patch:writable parameters) are available for queries.
    const LilvNode* bundle_uri = lilv_plugin_get_bundle_uri(plugin);
    if (bundle_uri) {
        lilv_world_load_bundle(world, bundle_uri);
        lilv_world_load_resource(world, lilv_plugin_get_uri(plugin));
    }
    
    // Diagnostic: list plugin info before instantiation
    {
        const LilvNodes* req = lilv_plugin_get_required_features(plugin);
        const LilvNodes* opt = lilv_plugin_get_optional_features(plugin);
        if (req) {
            LILV_FOREACH(nodes, it, req) {
                const LilvNode* n = lilv_nodes_get(req, it);
                if (n && lilv_node_is_uri(n)) {
                    Trace::Debug("LV2Instrument: plugin %s requires feature %s", pluginURI_, lilv_node_as_uri(n));
                }
            }
            lilv_nodes_free((LilvNodes*)req);
        }
        if (opt) {
            LILV_FOREACH(nodes, it2, opt) {
                const LilvNode* n = lilv_nodes_get(opt, it2);
                if (n && lilv_node_is_uri(n)) {
                    Trace::Debug("LV2Instrument: plugin %s optional feature %s", pluginURI_, lilv_node_as_uri(n));
                }
            }
            lilv_nodes_free((LilvNodes*)opt);
        }

        // Log audio ports and resize info
        uint32_t np = lilv_plugin_get_num_ports(plugin);
        LilvNode* audio_uri = lilv_new_uri(static_cast<LilvWorld*>(world_), LILV_URI_AUDIO_PORT);
        LilvNode* output_uri = lilv_new_uri(static_cast<LilvWorld*>(world_), LILV_URI_OUTPUT_PORT);
        for (uint32_t i = 0; i < np; ++i) {
            const LilvPort* port = lilv_plugin_get_port_by_index(plugin, i);
            if (lilv_port_is_a(plugin, port, audio_uri)) {
                bool isOutput = lilv_port_is_a(plugin, port, output_uri);
                // Check for resize properties
                LilvNode* rsz_uri = lilv_new_uri(static_cast<LilvWorld*>(world_), "http://lv2plug.in/ns/ext/resize-port#minimumSize");
                const LilvNodes* rsz_nodes = lilv_port_get_value(plugin, port, rsz_uri);
                if (rsz_nodes && lilv_nodes_size(rsz_nodes) > 0) {
                    lilv_nodes_free((LilvNodes*)rsz_nodes);
                }
                lilv_node_free(rsz_uri);
                (void)isOutput;
            }
        }
        lilv_node_free(audio_uri);
        lilv_node_free(output_uri);
    }

    // Initialize URIDs if not already done
    if (g_midiEventUrid == 0) {
        g_midiEventUrid = urid_map(nullptr, LV2_MIDI__MidiEvent);
        g_atomSequenceUrid = urid_map(nullptr, LV2_ATOM__Sequence);
    }
    
    // Ensure g_atomFloatUrid is initialized
    if (g_atomFloatUrid == 0) {
        g_atomFloatUrid = urid_map(nullptr, LV2_ATOM__Float);
    }
    
    // Ensure options array is initialized with URIDs/values
    init_options_array();

    // Instantiate the plugin with 44100 Hz sample rate and our features


    LilvInstance* instance = lilv_plugin_instantiate(plugin, 44100.0, g_features);
    if (!instance) {
        Trace::Error("LV2Instrument: Failed to instantiate plugin with full features: %s", pluginURI_);

        // Diagnostic: check plugin library URI and attempt a direct dlopen to capture dlerror
        const LilvNode* lib_node = lilv_plugin_get_library_uri(plugin);
        const char* lib_uri = lib_node ? lilv_node_as_uri(lib_node) : nullptr;
        if (lib_uri) {
            Trace::Debug("LV2Instrument: plugin %s library uri=%s", pluginURI_, lib_uri);
            // Simple file:// -> path conversion
            const char *path = lib_uri;
            const char *prefix = "file://";
            if (strncmp(lib_uri, prefix, strlen(prefix)) == 0) {
                path = lib_uri + strlen(prefix);
            }

            // Try dlopen to get a clearer error
            void *h = dlopen(path, RTLD_NOW);
            if (!h) {
                const char *err = dlerror();
                Trace::Error("LV2Instrument: dlopen failed for %s: %s", path, err ? err : "<no error>");
            } else {
                Trace::Debug("LV2Instrument: dlopen succeeded for %s (closing)", path);
                dlclose(h);
            }
        } else {
            Trace::Debug("LV2Instrument: plugin %s has no library URI", pluginURI_);
        }

        // Try again with only URID map/unmap to see if extra features are causing the failure
        const LV2_Feature* minimal_features[] = { &g_mapFeature, &g_unmapFeature, nullptr };

        LilvInstance* inst2 = lilv_plugin_instantiate(plugin, 44100.0, minimal_features);
        if (!inst2) {
            // Also dump plugin manifest path hint if available (bundle path)
            const LilvNode* uri_node = lilv_plugin_get_uri(plugin);
            const char *bundle = uri_node ? lilv_node_as_uri(uri_node) : nullptr;
            Trace::Error("LV2Instrument: Failed to instantiate plugin %s even with minimal features; bundle=%s", pluginURI_, bundle ? bundle : "<none>");

            cleanupPlugin();
            return;
        } else {

            pluginInstance_ = inst2;
            Trace::Error("LV2Instrument: WARNING - Instantiated plugin with MINIMAL features only (no options/bufsize): %s", pluginURI_);
            return;
        }
    }

    pluginInstance_ = instance;
    Trace::Debug("LV2Instrument: Successfully instantiated plugin with FULL features: %s", pluginURI_);
}

void LV2Instrument::discoverParameters() {
    parameters_.clear();
    controlValues_.clear();
    audioInputPortL_ = -1;
    audioInputPortR_ = -1;
    audioOutputPortL_ = -1;
    audioOutputPortR_ = -1;
    midiInputPort_ = -1;
    audioOutputPorts_.clear();
    
    if (!plugin_ || !world_) {
        return;
    }
    
    const LilvPlugin* plugin = static_cast<const LilvPlugin*>(plugin_);

    // Get the number of ports
    uint32_t num_ports = lilv_plugin_get_num_ports(plugin);
    
    // Create URIs for port properties we care about
    LilvNode* input_class = lilv_new_uri(static_cast<LilvWorld*>(world_), LILV_URI_INPUT_PORT);
    LilvNode* output_class = lilv_new_uri(static_cast<LilvWorld*>(world_), LILV_URI_OUTPUT_PORT);
    LilvNode* control_class = lilv_new_uri(static_cast<LilvWorld*>(world_), LILV_URI_CONTROL_PORT);
    LilvNode* audio_class = lilv_new_uri(static_cast<LilvWorld*>(world_), LILV_URI_AUDIO_PORT);
    LilvNode* atom_port = lilv_new_uri(static_cast<LilvWorld*>(world_), "http://lv2plug.in/ns/ext/atom#AtomPort");
    LilvNode* midi_event = lilv_new_uri(static_cast<LilvWorld*>(world_), "http://lv2plug.in/ns/ext/midi#MidiEvent");
    
    // Counters for diagnostics
    int controlParamsFound = 0;
    int resourceParamsFound = 0;

    // Iterate through all ports
    for (uint32_t i = 0; i < num_ports; i++) {
        const LilvPort* port = lilv_plugin_get_port_by_index(plugin, i);
        
        // Check for an atom input port which we will use as the primary atom input (MIDI/patch input)
        if (lilv_port_is_a(plugin, port, atom_port) && lilv_port_is_a(plugin, port, input_class)) {
            // Use the first atom input port as the default MIDI/patch input port
            if (midiInputPort_ == -1) {
                midiInputPort_ = i;
                Trace::Debug("LV2Instrument: Selected atom input port %u as midi/patch input", i);
            }

            // Additionally check if it explicitly lists a buffer type (older plugins); log for diagnostics
            LilvNode* buffer_type_uri = lilv_new_uri(static_cast<LilvWorld*>(world_), "http://lv2plug.in/ns/ext/atom#bufferType");
            const LilvNodes* buf_types = lilv_port_get_value(plugin, port, buffer_type_uri);
            if (buf_types) {
                Trace::Debug("LV2Instrument: Atom input port %u advertises bufferType", i);
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
                // Collect all audio output ports so we can optionally only connect stereo
                audioOutputPorts_.push_back(i);
                if (audioOutputPortL_ == -1) {
                    audioOutputPortL_ = i;
                } else if (audioOutputPortR_ == -1) {
                    audioOutputPortR_ = i;
                }
            }
        }
        
        // Check if this is a control port (accept input or output directions)
        // Note: atom ports with lv2:designation lv2:control are transport/MIDI ports,
        // NOT user-controllable parameters — do not add them as parameters.
        bool isControlPort = lilv_port_is_a(plugin, port, control_class);

        if (isControlPort) {
            
            LV2PluginParameter param;
            param.variable = nullptr;  // Initialize to null
            param.isOutput = lilv_port_is_a(plugin, port, output_class); // remember direction
            // Log basic port characteristics for diagnostic purposes
            const LilvNode* pname = lilv_port_get_name(plugin, port);
            if (pname) lilv_node_free((LilvNode*)pname);
            
            // Get port label (preferred over name for UI display)
            LilvNode* label_uri = lilv_new_uri(static_cast<LilvWorld*>(world_), "http://www.w3.org/2000/01/rdf-schema#label");
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
            LilvNode* group_uri = lilv_new_uri(static_cast<LilvWorld*>(world_), "http://lv2plug.in/ns/ext/port-groups#group");
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
            LilvNode* port_property_uri = lilv_new_uri(static_cast<LilvWorld*>(world_), "http://lv2plug.in/ns/lv2core#portProperty");
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
            // Mark whether this control port is actually an atom port (e.g., an AtomPort with lv2:designation lv2:control)
            param.isAtomPort = lilv_port_is_a(plugin, port, atom_port);

            // If this is an atom-designated control input port and no resource was found, synthesize a fallback resource URI
            if (param.isAtomPort && !param.isOutput && param.resourceURI.empty()) {
                // Use pluginURI#<sanitized-port-name> as a best-effort subject
                std::string sanitized = param.name;
                for (auto &ch : sanitized) if (isspace((unsigned char)ch)) ch = '_';
                param.resourceURI = std::string(pluginURI_) + "#" + sanitized;
                Trace::Debug("LV2Instrument: Falling back to synthesized resource URI=%s for atom control input port name=%s", param.resourceURI.c_str(), param.name.c_str());
            }

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
            if (scaledValue < 0) {
                scaledValue = 0;
            }
            if (scaledValue > 127) {
                scaledValue = 127;
            }
            
            // Use WatchedVariable so the instrument receives notifications when UI changes the variable
            WatchedVariable *wv = new WatchedVariable(varName, MAKE_FOURCC('L','P', (int)parameters_.size()/256, (int)parameters_.size()%256), scaledValue);
            Insert(wv);
            param.variable = (Variable*)wv;
            // Instrument observes the variable to react immediately to user changes
            wv->AddObserver(*this);
            
            lilv_node_free(def_node);
            lilv_node_free(min_node);
            lilv_node_free(max_node);
            
            parameters_.push_back(param);
            
            // Allocate storage for this control value
            controlValues_.push_back(param.defaultValue);

            // Diagnostic: log parameter info
            Trace::Debug("LV2Instrument: param discovered name=%s portIndex=%d isAtom=%d resourceURI=%s", param.name.c_str(), param.portIndex, param.isAtomPort ? 1 : 0, param.resourceURI.c_str());

            controlParamsFound++;

            // Diagnostic dump for braids/plaits to inspect scale points and ranges (helps debug crashes)
            if (param.groupName == "braids" || param.groupName == "plaits") {
                for (size_t sp=0; sp<param.scalePoints.size(); ++sp) {
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
        // Allocate a larger MIDI/Atom buffer to satisfy plugins that request large
        // minimum atom buffer sizes (some plugins require >8KB or tens of KB).
        midiBufferSize_ = 65536;
        midiBuffer_ = new uint8_t[midiBufferSize_];
    }

    // Discover patch:writable parameters — these are resource-backed params controlled
    // via patch:Set atom messages (used by JUCE-based plugins like JC303, ChowKick, Surge XT, etc.)
    LilvNode* pw_pred = lilv_new_uri(static_cast<LilvWorld*>(world_), LV2_PATCH__writable);
    const LilvNodes* writable_nodes = lilv_world_find_nodes(static_cast<LilvWorld*>(world_),
        lilv_plugin_get_uri(plugin), pw_pred, NULL);
    if (writable_nodes && lilv_nodes_size(writable_nodes) > 0) {
        Trace::Debug("LV2Instrument: Found %u patch:writable parameters", lilv_nodes_size(writable_nodes));
        LILV_FOREACH(nodes, wit, writable_nodes) {
            const LilvNode* pnode = lilv_nodes_get(writable_nodes, wit);
            if (!lilv_node_is_uri(pnode)) continue;

            const char* paramResourceURI = lilv_node_as_uri(pnode);

            // Get label
            LilvNode* label_uri = lilv_new_uri(static_cast<LilvWorld*>(world_), "http://www.w3.org/2000/01/rdf-schema#label");
            const LilvNodes* label_nodes = lilv_world_find_nodes(static_cast<LilvWorld*>(world_), pnode, label_uri, NULL);
            std::string pname;
            if (label_nodes && lilv_nodes_size(label_nodes) > 0) {
                pname = lilv_node_as_string(lilv_nodes_get_first(label_nodes));
                lilv_nodes_free((LilvNodes*)label_nodes);
            } else {
                // Fallback to URI fragment
                const char* colon = strrchr(paramResourceURI, ':');
                const char* hash = strrchr(paramResourceURI, '#');
                const char* slash = strrchr(paramResourceURI, '/');
                if (hash) pname = hash + 1;
                else if (colon) pname = colon + 1;
                else if (slash) pname = slash + 1;
                else pname = paramResourceURI;
            }
            lilv_node_free(label_uri);

            // Skip if we already discovered a parameter with the same name or URI
            bool exists = false;
            for (size_t pi = 0; pi < parameters_.size(); ++pi) {
                if (parameters_[pi].name == pname || parameters_[pi].resourceURI == paramResourceURI) {
                    exists = true; break;
                }
            }
            if (exists) continue;

            LV2PluginParameter param;
            param.name = pname;
            param.groupName = "";
            param.resourceURI = paramResourceURI;

            // Group
            LilvNode* group_uri = lilv_new_uri(static_cast<LilvWorld*>(world_), "http://lv2plug.in/ns/ext/port-groups#group");
            const LilvNodes* group_nodes = lilv_world_find_nodes(static_cast<LilvWorld*>(world_), pnode, group_uri, NULL);
            if (group_nodes && lilv_nodes_size(group_nodes) > 0) {
                const LilvNode* gn = lilv_nodes_get_first(group_nodes);
                if (lilv_node_is_uri(gn)) {
                    const char* guri = lilv_node_as_uri(gn);
                    const char* gh = strrchr(guri, '#');
                    const char* gc = strrchr(guri, ':');
                    if (gh) param.groupName = std::string(gh + 1);
                    else if (gc) param.groupName = std::string(gc + 1);
                    else param.groupName = std::string(guri);
                }
                lilv_nodes_free((LilvNodes*)group_nodes);
            }
            lilv_node_free(group_uri);

            // Min/Max/Default values
            LilvNode* min_uri = lilv_new_uri(static_cast<LilvWorld*>(world_), "http://lv2plug.in/ns/lv2core#minimum");
            const LilvNodes* min_nodes = lilv_world_find_nodes(static_cast<LilvWorld*>(world_), pnode, min_uri, NULL);
            param.minValue = (min_nodes && lilv_nodes_size(min_nodes) > 0) ? lilv_node_as_float(lilv_nodes_get_first(min_nodes)) : 0.0f;
            if (min_nodes) lilv_nodes_free((LilvNodes*)min_nodes);
            lilv_node_free(min_uri);

            LilvNode* max_uri = lilv_new_uri(static_cast<LilvWorld*>(world_), "http://lv2plug.in/ns/lv2core#maximum");
            const LilvNodes* max_nodes = lilv_world_find_nodes(static_cast<LilvWorld*>(world_), pnode, max_uri, NULL);
            param.maxValue = (max_nodes && lilv_nodes_size(max_nodes) > 0) ? lilv_node_as_float(lilv_nodes_get_first(max_nodes)) : 1.0f;
            if (max_nodes) lilv_nodes_free((LilvNodes*)max_nodes);
            lilv_node_free(max_uri);

            LilvNode* def_uri = lilv_new_uri(static_cast<LilvWorld*>(world_), "http://lv2plug.in/ns/lv2core#default");
            const LilvNodes* def_nodes = lilv_world_find_nodes(static_cast<LilvWorld*>(world_), pnode, def_uri, NULL);
            param.defaultValue = (def_nodes && lilv_nodes_size(def_nodes) > 0) ? lilv_node_as_float(lilv_nodes_get_first(def_nodes)) :
                ((param.minValue >= 0.0f && param.maxValue <= 1.0f) ? 0.5f :
                 (param.minValue < 0.0f && param.maxValue > 0.0f ? 0.0f : param.minValue));
            if (def_nodes) lilv_nodes_free((LilvNodes*)def_nodes);
            lilv_node_free(def_uri);

            param.currentValue = param.defaultValue;
            param.portIndex = -1; // no direct control port — uses patch:Set
            param.variable = nullptr;

            // Create UI Variable
            std::string varDisplay = (param.groupName.empty() ? param.name : (param.groupName + ":" + param.name));
            char varName[64];
            if (varDisplay.length() > 20) {
                std::string shortName = varDisplay.substr(0, 17) + "...";
                snprintf(varName, 64, "%s", shortName.c_str());
            } else {
                snprintf(varName, 64, "%s", varDisplay.c_str());
            }
            int scaledValue = (int)(((param.currentValue - param.minValue) / (param.maxValue - param.minValue) * 127.0f) + 0.5f);
            if (scaledValue < 0) scaledValue = 0;
            if (scaledValue > 127) scaledValue = 127;

            WatchedVariable *wv2 = new WatchedVariable(varName, MAKE_FOURCC('L','P', (int)parameters_.size()/256, (int)parameters_.size()%256), scaledValue);
            Insert(wv2);
            param.variable = (Variable*)wv2;
            wv2->AddObserver(*this);
            parameters_.push_back(param);
            controlValues_.push_back(param.defaultValue);
            Trace::Debug("LV2Instrument: patch:writable param name=%s resourceURI=%s", param.name.c_str(), param.resourceURI.c_str());
            resourceParamsFound++;
        }
        lilv_nodes_free((LilvNodes*)writable_nodes);
    }
    lilv_node_free(pw_pred);

    // Discovery summary logging
    Trace::Debug("LV2Instrument: Discovered control params=%d resource params=%d total params=%d", controlParamsFound, resourceParamsFound, (int)parameters_.size());

    // Synthesize fallback resource URIs for parameters that have no direct control port and no explicit resource URI
    for (size_t pi = 0; pi < parameters_.size(); ++pi) {
        if (parameters_[pi].portIndex < 0 && parameters_[pi].resourceURI.empty()) {
            std::string sanitized = parameters_[pi].name;
            for (auto &ch : sanitized) if (isspace((unsigned char)ch)) ch = '_';
            parameters_[pi].resourceURI = std::string(pluginURI_) + "#" + sanitized;
            Trace::Debug("LV2Instrument: Synthesized fallback resource URI=%s for param name=%s portIndex=%d", parameters_[pi].resourceURI.c_str(), parameters_[pi].name.c_str(), parameters_[pi].portIndex);
        }
    }

    // Push initial parameter values into control port storage so the plugin
    // sees correct defaults. Do NOT queue patch:Set atom events here — those
    // would be sent on the first render and flood the atom buffer, potentially
    // preventing the plugin from processing MIDI events.
    for (size_t pi = 0; pi < parameters_.size(); ++pi) {
        int pidx = parameters_[pi].portIndex;
        if (pidx >= 0 && !parameters_[pi].isAtomPort) {
            if (pidx < (int)portControlStorage_.size()) {
                portControlStorage_[pidx] = parameters_[pi].currentValue;
            }
        }
    }
}



void LV2Instrument::SetForcedOutputChannels(int count) {
    forcedOutputChannels_ = count;
}

void LV2Instrument::connectPorts(int bufferSize) {
    if (!plugin_ || !pluginInstance_) {
        Trace::Error("LV2Instrument: Cannot connect ports, plugin or instance is null");
        return;
    }

    const uint32_t numPorts = lilv_plugin_get_num_ports(static_cast<const LilvPlugin*>(plugin_));

    // Prepare common lilv node types for classification
    LilvNode* audio_class = lilv_new_uri(static_cast<LilvWorld*>(world_), LILV_URI_AUDIO_PORT);
    LilvNode* control_class = lilv_new_uri(static_cast<LilvWorld*>(world_), LILV_URI_CONTROL_PORT);
    LilvNode* input_class = lilv_new_uri(static_cast<LilvWorld*>(world_), LILV_URI_INPUT_PORT);
    LilvNode* output_class = lilv_new_uri(static_cast<LilvWorld*>(world_), LILV_URI_OUTPUT_PORT);
    LilvNode* atom_class = lilv_new_uri(static_cast<LilvWorld*>(world_), "http://lv2plug.in/ns/ext/atom#AtomPort");

    // Ensure atom buffers vector is sized to number of ports
    atomInputBuffers_.assign(numPorts, nullptr);
    atomInputBufferSizes_.assign(numPorts, 0);

    // Ensure port control storage can hold a value for every port
    if (portControlStorage_.size() < numPorts) {
        portControlStorage_.resize(numPorts, 0.0f);
    }

    for (uint32_t i = 0; i < numPorts; ++i) {
        const LilvPort* port = lilv_plugin_get_port_by_index(static_cast<const LilvPlugin*>(plugin_), i);
        if (!port) {
            Trace::Error("LV2Instrument: Port %u is null", i);
            continue;
        }

        // Audio ports
        if (lilv_port_is_a(static_cast<const LilvPlugin*>(plugin_), port, audio_class)) {
            if (lilv_port_is_a(static_cast<const LilvPlugin*>(plugin_), port, input_class)) {
                // Connect plugin audio input ports to our audio input buffers
                if ((int)i == audioInputPortL_ && audioInputL_) {
                    lilv_instance_connect_port(static_cast<LilvInstance*>(pluginInstance_), i, audioInputL_);
                    Trace::Debug("LV2Instrument: Connected audio input L port %u", i);
                } else if ((int)i == audioInputPortR_ && audioInputR_) {
                    lilv_instance_connect_port(static_cast<LilvInstance*>(pluginInstance_), i, audioInputR_);
                    Trace::Debug("LV2Instrument: Connected audio input R port %u", i);
                } else {
                    // Connect to a zeroed input buffer to be safe
                    lilv_instance_connect_port(static_cast<LilvInstance*>(pluginInstance_), i, audioInputL_ ? audioInputL_ : (void*)nullptr);
                    Trace::Debug("LV2Instrument: Connected audio input (fallback) port %u", i);
                }
            } else if (lilv_port_is_a(static_cast<const LilvPlugin*>(plugin_), port, output_class)) {
                // Connect plugin audio output ports to our audio output buffers
                if ((int)i == audioOutputPortL_ && audioBufferL_) {
                    lilv_instance_connect_port(static_cast<LilvInstance*>(pluginInstance_), i, audioBufferL_);
                    Trace::Debug("LV2Instrument: Connected audio output L port %u -> audioBufferL_=%p", i, (void*)audioBufferL_);
                } else if ((int)i == audioOutputPortR_ && audioBufferR_) {
                    lilv_instance_connect_port(static_cast<LilvInstance*>(pluginInstance_), i, audioBufferR_);
                    Trace::Debug("LV2Instrument: Connected audio output R port %u -> audioBufferR_=%p", i, (void*)audioBufferR_);
                } else {
                    // Connect extra audio outputs to a separate dummy buffer so they
                    // don't overwrite the real L/R output data.
                    lilv_instance_connect_port(static_cast<LilvInstance*>(pluginInstance_), i, audioDummyBuffer_ ? audioDummyBuffer_ : (void*)nullptr);
                    Trace::Debug("LV2Instrument: Connected audio output (DUMMY) port %u -> audioDummyBuffer_=%p", i, (void*)audioDummyBuffer_);
                }
            }
            continue;
        }

        // Control ports
        if (lilv_port_is_a(static_cast<const LilvPlugin*>(plugin_), port, control_class)) {
            // Ensure storage exists for this port
            if (portControlStorage_.size() <= i) portControlStorage_.resize(i + 1, 0.0f);
            lilv_instance_connect_port(static_cast<LilvInstance*>(pluginInstance_), i, &portControlStorage_[i]);
            Trace::Debug("LV2Instrument: Connected control port %u to storage %p", i, (void*)&portControlStorage_[i]);
            continue;
        }

        // Atom ports (MIDI / Atom sequences)
        if (lilv_port_is_a(static_cast<const LilvPlugin*>(plugin_), port, atom_class)) {
            bool isAtomInput = lilv_port_is_a(static_cast<const LilvPlugin*>(plugin_), port, input_class);
            bool isAtomOutput = lilv_port_is_a(static_cast<const LilvPlugin*>(plugin_), port, output_class);
            Trace::Debug("LV2Instrument: Connecting atom port %u (input=%d output=%d)", i, isAtomInput, isAtomOutput);

            // MIDI input port uses preallocated midiBuffer_
            if ((int)i == midiInputPort_ && midiBuffer_) {
                lilv_instance_connect_port(static_cast<LilvInstance*>(pluginInstance_), i, midiBuffer_);
                Trace::Debug("LV2Instrument: Connected MIDI input port %u", i);
                continue;
            }

            size_t bufSize = (midiBufferSize_ > 0) ? midiBufferSize_ : 8192;

            if (isAtomOutput) {
                // Atom OUTPUT port: use atomOutputBuffers_ so we can re-init them each render cycle
                if (atomOutputBuffers_.size() <= i) {
                    atomOutputBuffers_.resize(i + 1, nullptr);
                    atomOutputBufferSizes_.resize(i + 1, 0);
                }
                if (atomOutputBuffers_[i]) {
                    delete[] atomOutputBuffers_[i];
                }
                atomOutputBuffers_[i] = new uint8_t[bufSize];
                atomOutputBufferSizes_[i] = bufSize;
                // Initialize as empty sequence with full capacity
                LV2_Atom_Sequence* seq = (LV2_Atom_Sequence*)atomOutputBuffers_[i];
                seq->atom.type = g_atomSequenceUrid;
                seq->atom.size = bufSize - sizeof(LV2_Atom);  // capacity for plugin to write into
                seq->body.unit = 0;
                seq->body.pad = 0;
                lilv_instance_connect_port(static_cast<LilvInstance*>(pluginInstance_), i, atomOutputBuffers_[i]);
                Trace::Debug("LV2Instrument: Connected atom OUTPUT port %u size=%zu", i, bufSize);
            } else {
                // Atom INPUT port (non-MIDI): allocate and connect
                if (atomInputBuffers_[i]) {
                    delete[] atomInputBuffers_[i];
                    atomInputBuffers_[i] = nullptr;
                    atomInputBufferSizes_[i] = 0;
                }
                atomInputBuffers_[i] = new uint8_t[bufSize];
                memset(atomInputBuffers_[i], 0, bufSize);
                atomInputBufferSizes_[i] = bufSize;
                lilv_instance_connect_port(static_cast<LilvInstance*>(pluginInstance_), i, atomInputBuffers_[i]);
                Trace::Debug("LV2Instrument: Connected atom INPUT port %u size=%zu", i, bufSize);
            }
            continue; 
        }


    }

    // Write parameter default values into port control storage so the plugin
    // sees correct initial values instead of 0.0f on every port.
    for (size_t pi = 0; pi < parameters_.size(); ++pi) {
        int pidx = parameters_[pi].portIndex;
        if (pidx >= 0 && pidx < (int)portControlStorage_.size() && !parameters_[pi].isAtomPort) {
            portControlStorage_[pidx] = parameters_[pi].currentValue;
            Trace::Debug("LV2Instrument: connectPorts default param[%zu] port=%d name=%s value=%.4f", pi, pidx, parameters_[pi].name.c_str(), parameters_[pi].currentValue);
        }
    }

    Trace::Debug("LV2Instrument: connectPorts summary: audioOutL=%d audioOutR=%d audioInL=%d audioInR=%d midiIn=%d",
        audioOutputPortL_, audioOutputPortR_, audioInputPortL_, audioInputPortR_, midiInputPort_);

    // Free temporary nodes
    lilv_node_free(audio_class);
    lilv_node_free(control_class);
    lilv_node_free(input_class);
    lilv_node_free(output_class);
    lilv_node_free(atom_class);

}

void LV2Instrument::SetParameterValue(int paramIndex, float value) {
    if (paramIndex >= 0 && paramIndex < (int)parameters_.size()) {
        parameters_[paramIndex].currentValue = value;
        if (paramIndex < (int)controlValues_.size()) {
            controlValues_[paramIndex] = value;
        }
        // Also sync to port indexed storage so connected plugin ports receive the new value
        int pidx = parameters_[paramIndex].portIndex;
        if (pidx >= 0) {
            if ((int)portControlStorage_.size() <= pidx) {
                // Safety: expand if needed (shouldn't normally happen)
                portControlStorage_.resize(pidx + 1, 0.0f);
            }

            // Prepare diagnostic information
            float* target = &portControlStorage_[pidx];
            uintptr_t tgtAddr = (uintptr_t)target;
            uintptr_t audL = (uintptr_t)audioBufferL_;
            uintptr_t audR = (uintptr_t)audioBufferR_;
            uintptr_t inL = (uintptr_t)audioInputL_;
            uintptr_t inR = (uintptr_t)audioInputR_;
            size_t bsz = (size_t)bufferSize_; 

            // Check if target falls into any known audio buffer range; if so, skip the write to prevent corruption
            bool overlap = false;
            if (bsz > 0) {
                size_t byteCount = bsz * sizeof(float);
                if (audioBufferL_ && tgtAddr >= audL && tgtAddr < (audL + byteCount)) overlap = true;
                if (audioBufferR_ && tgtAddr >= audR && tgtAddr < (audR + byteCount)) overlap = true;
                if (audioInputL_ && tgtAddr >= inL && tgtAddr < (inL + byteCount)) overlap = true;
                if (audioInputR_ && tgtAddr >= inR && tgtAddr < (inR + byteCount)) overlap = true;
            }

            // Also check explicit audio port indices to avoid writing to those
            if (pidx == audioOutputPortL_ || pidx == audioOutputPortR_ || pidx == audioInputPortL_ || pidx == audioInputPortR_) {
                overlap = true;
            }

            if (overlap) {
                Trace::Error("LV2Instrument: SetParameterValue SKIPPING write: target overlaps audio buffer or is audio port! paramIndex=%d portIndex=%d name=%s", paramIndex, pidx, parameters_[paramIndex].name.c_str());
            } else {
                portControlStorage_[pidx] = value; 
            }

            // Handle atom ports
            if (parameters_[paramIndex].isAtomPort) {
                int portIndex = parameters_[paramIndex].portIndex;
                if (portIndex >= 0 && portIndex < (int)atomInputBuffers_.size() && atomInputBuffers_[portIndex]) {
                    // Construct a proper LV2_Atom_Sequence containing one LV2_Atom_Event with a float body
                    uint8_t* buf = atomInputBuffers_[portIndex];
                    size_t cap = atomInputBufferSizes_[portIndex];
                    if (cap >= sizeof(LV2_Atom_Sequence) + sizeof(LV2_Atom_Event) + sizeof(float)) {
                        LV2_Atom_Sequence* seq = (LV2_Atom_Sequence*)buf;
                        seq->atom.size = sizeof(LV2_Atom_Sequence_Body) + sizeof(LV2_Atom_Event) + sizeof(float);
                        seq->atom.type = g_atomSequenceUrid ? g_atomSequenceUrid : urid_map(nullptr, LV2_ATOM__Sequence);
                        seq->body.unit = 0;
                        seq->body.pad = 0;

                        uint8_t* b = (uint8_t*)LV2_ATOM_CONTENTS(LV2_Atom_Sequence, seq);
                        LV2_Atom_Event* ev = (LV2_Atom_Event*)b;
                        ev->time.frames = 0;
                        ev->body.size = sizeof(float);
                        ev->body.type = g_atomFloatUrid ? g_atomFloatUrid : urid_map(nullptr, LV2_ATOM__Float);
                        memcpy(LV2_ATOM_BODY(&ev->body), &value, sizeof(float));

                        Trace::Debug("LV2Instrument: Wrote float event to atom port %d paramIndex=%d value=%f", portIndex, paramIndex, value);
                    } else {
                        Trace::Error("LV2Instrument: atom buffer too small for float event on port %d", portIndex);
                    }
                }
            } else {
            }
        } else {
            // No direct control port - send a patch:Set message if we have a resource URI and an atom input port
            if (!parameters_[paramIndex].resourceURI.empty() && midiInputPort_ >= 0 && midiBuffer_) {
                LV2_Atom_Forge forge;
                lv2_atom_forge_init(&forge, &g_uridMapFeature);

                const size_t TMP_SIZE = 1024;
                uint8_t tmp[TMP_SIZE];
                lv2_atom_forge_set_buffer(&forge, tmp, TMP_SIZE);

                LV2_Atom_Forge_Frame frame;
                // Create a patch:Set object per LV2 Patch specification
                lv2_atom_forge_object(&forge, &frame, 0, urid_map(nullptr, LV2_PATCH__Set));
                // patch:property — identifies WHICH parameter to set (the parameter's resource URI)
                lv2_atom_forge_key(&forge, urid_map(nullptr, LV2_PATCH__property));
                lv2_atom_forge_urid(&forge, urid_map(nullptr, parameters_[paramIndex].resourceURI.c_str()));
                // patch:value — the new value
                lv2_atom_forge_key(&forge, urid_map(nullptr, LV2_PATCH__value));
                lv2_atom_forge_float(&forge, value);
                lv2_atom_forge_pop(&forge, &frame);

                if (forge.offset >= sizeof(LV2_Atom)) {
                    LV2_Atom* atom = (LV2_Atom*)tmp;
                    uint32_t bodySize = atom->size;
                    uint32_t atomType = atom->type;

                    PendingAtomEvent ae;
                    ae.type = atomType;
                    ae.data.resize(bodySize);
                    memcpy(ae.data.data(), LV2_ATOM_BODY(atom), bodySize);
                    ae.destPortIndex = midiInputPort_ >= 0 ? midiInputPort_ : -1;
                    if (parameters_[paramIndex].portIndex >= 0 && parameters_[paramIndex].isAtomPort) {
                        ae.destPortIndex = parameters_[paramIndex].portIndex;
                    }
                    pendingAtomEvents_.push_back(std::move(ae));
                    Trace::Debug("LV2Instrument: Queued patch:Set for param=%s uri=%s value=%f",
                                 parameters_[paramIndex].name.c_str(),
                                 parameters_[paramIndex].resourceURI.c_str(), value);
                }
            }
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

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

// Minimal options array (zero-terminated) for LV2_OPTIONS__options feature
// We'll provide explicit entries for min, nominal and max block lengths plus a zero terminator.
static uint32_t s_minBlock = 64u;
static uint32_t s_nominalBlock = 1024u;
static uint32_t s_maxBlock = 131072u;
static LV2_Options_Option g_optionsArray[4] = {
    { (LV2_Options_Context)0, 0u, 0u, 0u, 0u, nullptr }, // max block length (filled at runtime)
    { (LV2_Options_Context)0, 0u, 0u, 0u, 0u, nullptr }, // nominal block length (filled at runtime)
    { (LV2_Options_Context)0, 0u, 0u, 0u, 0u, nullptr }, // min block length (filled at runtime)
    { (LV2_Options_Context)0, 0u, 0u, 0u, 0u, nullptr }  // zero terminator
};
static LV2_Feature g_optionsFeature = { LV2_OPTIONS__options, (void*)g_optionsArray };

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

static LV2_Options_Interface g_optionsInterface = { lv2_options_get, lv2_options_set };
static LV2_Feature g_optionsInterfaceFeature = { LV2_OPTIONS__interface, &g_optionsInterface };

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
static LV2_Feature g_boundedFeature = { LV2_BUF_SIZE__boundedBlockLength, &g_boundedBlockLength };

static const LV2_Feature* g_features[] = { &g_mapFeature, &g_unmapFeature, &g_optionsFeature, &g_optionsInterfaceFeature, &g_boundedFeature, nullptr };

// Cached URIDs for performance
static LV2_URID g_midiEventUrid = 0;
static LV2_URID g_atomSequenceUrid = 0;

static LV2_URID g_atomFloatUrid = 0;

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

    // Temporary diagnostic injection: once per run, call SetParameterValue for the first parameter to exercise write path
    static bool debugInjected = false;
    if (!debugInjected && !parameters_.empty()) {
        // Bump the first parameter slightly to trigger patch/set or port write and observe behavior
        int pi = 0;
        float newVal = parameters_[pi].currentValue + (parameters_[pi].maxValue - parameters_[pi].minValue) * 0.1f;
        if (newVal > parameters_[pi].maxValue) newVal = parameters_[pi].maxValue;
        Trace::Debug("LV2Instrument: debug injection - forcing SetParameterValue for paramIndex=%d name=%s old=%g new=%g portIndex=%d isAtom=%d resourceURI=%s", pi, parameters_[pi].name.c_str(), parameters_[pi].currentValue, newVal, parameters_[pi].portIndex, parameters_[pi].isAtomPort ? 1 : 0, parameters_[pi].resourceURI.c_str());
        SetParameterValue(pi, newVal);
        debugInjected = true;
    }
    
    // Activate plugin once after first connection
    if (!isActivated_ && pluginInstance_) {
        LilvInstance* instance = (LilvInstance*)pluginInstance_;
        lilv_instance_activate(instance);
        isActivated_ = true;
        // Debug: initialize test note flag
        testNoteSent_ = false;

        // One-time debug injection after activation to exercise parameter update path
        static bool debugInjectedActivation = false;
        if (!debugInjectedActivation && !parameters_.empty()) {
            int pi = 0;
            float newVal = parameters_[pi].currentValue + (parameters_[pi].maxValue - parameters_[pi].minValue) * 0.1f;
            if (newVal > parameters_[pi].maxValue) newVal = parameters_[pi].maxValue;
            Trace::Debug("LV2Instrument: debug injection (post-activate) - forcing SetParameterValue for paramIndex=%d name=%s old=%g new=%g portIndex=%d isAtom=%d resourceURI=%s", pi, parameters_[pi].name.c_str(), parameters_[pi].currentValue, newVal, parameters_[pi].portIndex, parameters_[pi].isAtomPort ? 1 : 0, parameters_[pi].resourceURI.c_str());
            SetParameterValue(pi, newVal);
            debugInjectedActivation = true;
        }
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

            // Extra logging for braids shape crashes
            if (parameters_[i].groupName == "braids" && parameters_[i].name.find("Shape") != std::string::npos) {
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

        // Debug: if activation just happened and we haven't sent a test note yet, queue one
        if (isActivated_ && !testNoteSent_) {
            MidiEvent noteOn;
            noteOn.data[0] = 0x90; noteOn.data[1] = 60; noteOn.data[2] = 100; noteOn.size = 3;
            pendingMidiEvents_.push_back(noteOn);
            Trace::Debug("LV2Instrument: queued debug NoteOn (60) to verify output");
            testNoteSent_ = true;
        }
        
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
        }
        pendingMidiEvents_.clear();

        // Write pending atom events (grouped by destination port)
        if (!pendingAtomEvents_.empty()) {
            // Group events by dest port
            std::map<int, std::vector<PendingAtomEvent>> groups;
            for (auto &ae : pendingAtomEvents_) {
                groups[ae.destPortIndex].push_back(ae);
            }

            // Helper to write a vector of events into a buffer
            auto writeEventsToBuffer = [&](uint8_t* buffer, size_t bufCap, const std::vector<PendingAtomEvent> &events) {
                if (!buffer) return;
                LV2_Atom_Sequence* seq = (LV2_Atom_Sequence*)buffer;
                seq->atom.size = sizeof(LV2_Atom_Sequence_Body); // reset
                seq->atom.type = g_atomSequenceUrid ? g_atomSequenceUrid : urid_map(nullptr, LV2_ATOM__Sequence);
                seq->body.unit = 0;
                seq->body.pad = 0;

                uint8_t* b = (uint8_t*)LV2_ATOM_CONTENTS(LV2_Atom_Sequence, seq);
                size_t off = 0;
                size_t cap = bufCap - sizeof(LV2_Atom_Sequence);

                for (const auto &e : events) {
                    size_t required = sizeof(LV2_Atom_Event) + ((e.data.size() + 7) & ~7);
                    if (off + required > cap) break;
                    LV2_Atom_Event* event = (LV2_Atom_Event*)(b + off);
                    event->time.frames = 0;
                    event->body.size = (uint32_t)e.data.size();
                    event->body.type = e.type;
                    memcpy(LV2_ATOM_BODY(&event->body), e.data.data(), e.data.size());

                    // Diagnostic: dump the first few bytes and the event type
                    {
                        unsigned int show = (unsigned int)std::min<size_t>(e.data.size(), 16);
                        char dump[128]; int dp=0;
                        uint8_t *bd = (uint8_t*)LV2_ATOM_BODY(&event->body);
                        for (unsigned int xi=0; xi<show; ++xi) { dp += snprintf(dump+dp, sizeof(dump)-dp, "%02X", bd[xi]); if (xi<show-1) dump[dp++]=','; }
                        Trace::Debug("LV2Instrument: wrote atom event type=%u size=%u firstBytes=%s", (unsigned)e.type, (unsigned)e.data.size(), dump);
                    }

                    // Append full binary payload for offline inspection
                    {
                        static int writtenCounter = 0;
                        ++writtenCounter;
                        std::string wpath = std::string("/tmp/lv2_written_event_port_") + std::to_string(getpid()) + "_" + std::to_string(writtenCounter) + ".bin";
                        std::ofstream wofs(wpath, std::ios::binary);
                        if (wofs) {
                            wofs.write((char*)LV2_ATOM_BODY(&event->body), event->body.size);
                            Trace::Debug("LV2Instrument: dumped written event to %s (size=%u) for port write", wpath.c_str(), (unsigned)event->body.size);
                        }
                    }

                    size_t padded_size = sizeof(LV2_Atom_Event) + ((e.data.size() + 7) & ~7);
                    off += padded_size;
                }
                seq->atom.size = sizeof(LV2_Atom_Sequence_Body) + off;
            };

            // Write to MIDI/primary atom input if present
            auto it = groups.find(midiInputPort_);
            if (it != groups.end() && midiBuffer_) {
                writeEventsToBuffer(midiBuffer_, midiBufferSize_, it->second);
                Trace::Debug("LV2Instrument: wrote %u atom events to midiInputPort=%d", (unsigned)it->second.size(), midiInputPort_);
            }

            // Write to other atom input ports
            for (auto &g : groups) {
                int dest = g.first;
                if (dest == midiInputPort_) continue;
                if (dest >= 0 && dest < (int)atomInputBuffers_.size() && atomInputBuffers_[dest]) {
                    writeEventsToBuffer(atomInputBuffers_[dest], atomInputBufferSizes_[dest], g.second);
                    Trace::Debug("LV2Instrument: wrote %u atom events to atom input port=%d", (unsigned)g.second.size(), dest);
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
                    writeEventsToBuffer(midiBuffer_, midiBufferSize_, broadcastEvents);
                    Trace::Debug("LV2Instrument: broadcast wrote %u atom events to midiInputPort=%d", (unsigned)broadcastEvents.size(), midiInputPort_);
                }
                for (int port = 0; port < (int)atomInputBuffers_.size(); ++port) {
                    if (atomInputBuffers_[port]) {
                        writeEventsToBuffer(atomInputBuffers_[port], atomInputBufferSizes_[port], broadcastEvents);
                        Trace::Debug("LV2Instrument: broadcast wrote %u atom events to atom input port=%d", (unsigned)broadcastEvents.size(), port);
                    }
                }
            }
        }
        pendingAtomEvents_.clear();
        
        // Update final sequence size
        seq->atom.size = sizeof(LV2_Atom_Sequence_Body) + offset;

        // Diagnostic: print MIDI sequence summary
        Trace::Debug("LV2Instrument: midi seq size=%u offset=%u", seq->atom.size, (unsigned)offset);
        // Walk events and print their types/sizes for debugging
        size_t scan = 0;
        while (scan + sizeof(LV2_Atom_Event) <= offset) {
            LV2_Atom_Event* ev = (LV2_Atom_Event*)(buf + scan);
            uint32_t bsz = ev->body.size;
            LV2_URID btype = ev->body.type;
            Trace::Debug("LV2Instrument: midi event at %u type=%u size=%u", (unsigned)scan, (unsigned)btype, (unsigned)bsz);
            // Print up to first 8 bytes
            uint8_t *bd = (uint8_t*)LV2_ATOM_BODY(&ev->body);
            char dump[64]; int dp=0; for (uint32_t xi=0; xi<bsz && xi<8; ++xi) { dp += snprintf(dump+dp, sizeof(dump)-dp, "%02X", bd[xi]); if (xi<bsz-1 && xi<7) dump[dp++]=','; }
            Trace::Debug("LV2Instrument: midi event data: %s", dump);
            size_t padded_size = sizeof(LV2_Atom_Event) + ((bsz + 7) & ~7);
            scan += padded_size;
        }
    }

    // Run the plugin to generate output
    LilvInstance* instance = (LilvInstance*)pluginInstance_;

    // Debug: log braids shape parameter(s) before calling into the plugin
    for (size_t i = 0; i < parameters_.size() && i < controlValues_.size(); ++i) {
        if (parameters_[i].groupName == "braids" && parameters_[i].name.find("Shape") != std::string::npos) {
        }
    }

    // Diagnostic: dump buffer headers and pointers before running plugin
    if (midiBuffer_) {
        LV2_Atom_Sequence* seq = (LV2_Atom_Sequence*)midiBuffer_;
        Trace::Debug("LV2Instrument: MIDI buffer seq size=%u type=%u", seq->atom.size, seq->atom.type);
    } else {
        Trace::Debug("LV2Instrument: MIDI buffer is NULL");
    }
    for (size_t pi = 0; pi < atomInputBuffers_.size(); ++pi) {
        if (atomInputBuffers_[pi]) {
            LV2_Atom_Sequence* s = (LV2_Atom_Sequence*)atomInputBuffers_[pi];
            Trace::Debug("LV2Instrument: atom buffer[%zu]=%p size=%u type=%u cap=%zu", pi, atomInputBuffers_[pi], s->atom.size, s->atom.type, atomInputBufferSizes_[pi]);
        }
    }
    Trace::Debug("LV2Instrument: audioBufferL=%p audioBufferR=%p audioInputL=%p audioInputR=%p portControlStorage_ptr=%p", audioBufferL_, audioBufferR_, audioInputL_, audioInputR_, portControlStorage_.empty() ? nullptr : (void*)&portControlStorage_[0]);

    lilv_instance_run(instance, size);

    // Diagnostic: check if plugin wrote any non-zero samples into output buffers
    bool anyNonZero = false;
    if (audioBufferL_ && audioBufferR_) {
        for (int ii = 0; ii < size; ++ii) {
            if (fabsf(audioBufferL_[ii]) > 1e-10f || fabsf(audioBufferR_[ii]) > 1e-10f) { anyNonZero = true; break; }
        }
    }
    Trace::Debug("LV2Instrument: render post-run anyNonZero=%d firstSamples L0=%g R0=%g", anyNonZero ? 1 : 0, audioBufferL_ ? audioBufferL_[0] : 0.0f, audioBufferR_ ? audioBufferR_[0] : 0.0f);

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

        // Initialize resource-backed parameters by sending their default via patch:Set
        int initialized = 0;
        for (size_t pi = 0; pi < parameters_.size(); ++pi) {
            if (parameters_[pi].portIndex < 0 && !parameters_[pi].resourceURI.empty()) {
                SetParameterValue((int)pi, parameters_[pi].currentValue);
                initialized++;
            }
        }
        Trace::Debug("LV2Instrument: Sent initial patch:Set for %d resource params", initialized);


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
        int audioOutputs = 0;
        for (uint32_t i = 0; i < np; ++i) {
            const LilvPort* port = lilv_plugin_get_port_by_index(plugin, i);
            if (lilv_port_is_a(plugin, port, lilv_new_uri(static_cast<LilvWorld*>(world_), LILV_URI_AUDIO_PORT))) {
                bool isOutput = lilv_port_is_a(plugin, port, lilv_new_uri(static_cast<LilvWorld*>(world_), LILV_URI_OUTPUT_PORT));
                if (isOutput) audioOutputs++;
                // Check for resize properties
                LilvNode* rsz_uri = lilv_new_uri(static_cast<LilvWorld*>(world_), "http://lv2plug.in/ns/ext/resize-port#minimumSize");
                const LilvNodes* rsz_nodes = lilv_port_get_value(plugin, port, rsz_uri);
                if (rsz_nodes && lilv_nodes_size(rsz_nodes) > 0) {
                    lilv_nodes_free((LilvNodes*)rsz_nodes);
                }
                lilv_node_free(rsz_uri);
            }
        }

        if (audioOutputs > 2 && forcedOutputChannels_ == 0) {
            forcedOutputChannels_ = 2;
        }
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
            Trace::Debug("LV2Instrument: Instantiated plugin (minimal features): %s", pluginURI_);
            return;
        }
    }

    pluginInstance_ = instance;
    Trace::Debug("LV2Instrument: Successfully instantiated plugin: %s", pluginURI_);
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
    forcedOutputChannels_ = 0;
    
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
        bool isControlPort = lilv_port_is_a(plugin, port, control_class);
        // Also treat atom ports with lv2:designation == lv2:control as control ports
        if (!isControlPort && lilv_port_is_a(plugin, port, atom_port)) {
            LilvNode* des_uri = lilv_new_uri(static_cast<LilvWorld*>(world_), "http://lv2plug.in/ns/lv2core#designation");
            const LilvNodes* des_nodes = lilv_port_get_value(plugin, port, des_uri);
            if (des_nodes && lilv_nodes_size(des_nodes) > 0) {
                const LilvNode* dn = lilv_nodes_get_first(des_nodes);
                if (dn && lilv_node_is_uri(dn) && strcmp(lilv_node_as_uri(dn), "http://lv2plug.in/ns/lv2core#control") == 0) {
                    isControlPort = true;
                }
                lilv_nodes_free((LilvNodes*)des_nodes);
            }
            lilv_node_free(des_uri);
        }

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

    // Also scan for lv2:Parameter resources in the plugin's RDF (always perform this - avoids missing patch-based params)
    LilvNode* param_type = lilv_new_uri(static_cast<LilvWorld*>(world_), "http://lv2plug.in/ns/lv2core#Parameter");
    LilvNodes* param_nodes = lilv_plugin_get_related(plugin, param_type);
    if (param_nodes && lilv_nodes_size(param_nodes) > 0) {

        LilvIter *piter = lilv_nodes_begin(param_nodes);
        while (!lilv_nodes_is_end(param_nodes, piter)) {
            const LilvNode* pnode = lilv_nodes_get(param_nodes, piter);

            // Get label
            LilvNode* label_uri = lilv_new_uri(static_cast<LilvWorld*>(world_), "http://www.w3.org/2000/01/rdf-schema#label");
            const LilvNodes* label_nodes = lilv_world_find_nodes(static_cast<LilvWorld*>(world_), pnode, label_uri, NULL);
            std::string pname;
            if (label_nodes && lilv_nodes_size(label_nodes) > 0) {
                const LilvNode* ln = lilv_nodes_get_first(label_nodes);
                pname = lilv_node_as_string(ln);
                lilv_nodes_free((LilvNodes*)label_nodes);
            } else {
                // Fallback to URI fragment or node string
                if (lilv_node_is_uri(pnode)) {
                    const char* uri = lilv_node_as_uri(pnode);
                    const char* slash = strrchr(uri, '/');
                    pname = (slash ? slash + 1 : uri);
                } else {
                    pname = lilv_node_as_string(pnode);
                }
            }

            // Skip if we already discovered a parameter with the same name
            bool exists = false;
            for (size_t pi = 0; pi < parameters_.size(); ++pi) {
                if (parameters_[pi].name == pname) { exists = true; break; }
            }
            if (exists) { piter = lilv_nodes_next(param_nodes, piter); continue; }

            LV2PluginParameter param;
            param.name = pname;
            param.groupName = "";

            // Group
            LilvNode* group_uri = lilv_new_uri(static_cast<LilvWorld*>(world_), "http://lv2plug.in/ns/ext/port-groups#group");
            const LilvNodes* group_nodes = lilv_world_find_nodes(static_cast<LilvWorld*>(world_), pnode, group_uri, NULL);
            if (group_nodes && lilv_nodes_size(group_nodes) > 0) {
                const LilvNode* gn = lilv_nodes_get_first(group_nodes);
                if (lilv_node_is_uri(gn)) {
                    const char* guri = lilv_node_as_uri(gn);
                    const char* hash = strrchr(guri, '#');
                    const char* colon = strrchr(guri, ':');
                    if (hash) param.groupName = std::string(hash + 1);
                    else if (colon) param.groupName = std::string(colon + 1);
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
            param.defaultValue = (def_nodes && lilv_nodes_size(def_nodes) > 0) ? lilv_node_as_float(lilv_nodes_get_first(def_nodes)) : ((param.minValue >= 0.0f && param.maxValue <= 1.0f) ? 0.5f : (param.minValue < 0.0f && param.maxValue > 0.0f ? 0.0f : param.minValue));
            if (def_nodes) lilv_nodes_free((LilvNodes*)def_nodes);
            lilv_node_free(def_uri);

            param.currentValue = param.defaultValue;
            param.portIndex = -1; // no direct control port
            param.variable = nullptr;

            // If this is a discovered lv2:Parameter resource, record its URI so we can send patch:set messages later
            if (lilv_node_is_uri(pnode)) {
                const char *puri = lilv_node_as_uri(pnode);
                if (puri) {
                    param.resourceURI = puri;
                }
            }

            // Create UI Variable for this parameter so it appears in the UI
            std::string varDisplay = (param.groupName.empty() ? param.name : (param.groupName + ":" + param.name));
            char varName[64]; if (varDisplay.length()>20) { std::string shortName = varDisplay.substr(0,17)+"..."; snprintf(varName,64, "%s", shortName.c_str()); } else { snprintf(varName,64, "%s", varDisplay.c_str()); }
            int scaledValue = (int)(((param.currentValue - param.minValue) / (param.maxValue - param.minValue) * 127.0f) + 0.5f);
            if (scaledValue<0) {
                scaledValue = 0;
            }
            if (scaledValue > 127) {
                scaledValue = 127;
            }
            WatchedVariable *wv2 = new WatchedVariable(varName, MAKE_FOURCC('L','P', (int)parameters_.size()/256, (int)parameters_.size()%256), scaledValue);
            Insert(wv2);
            param.variable = (Variable*)wv2;
            wv2->AddObserver(*this);
            parameters_.push_back(param);
            controlValues_.push_back(param.defaultValue);
            Trace::Debug("LV2Instrument: resource param discovered name=%s resourceURI=%s portIndex=%d", param.name.c_str(), param.resourceURI.c_str(), param.portIndex);
            resourceParamsFound++;


            piter = lilv_nodes_next(param_nodes, piter);
        }
        lilv_nodes_free(param_nodes);
    }

    // If no lv2:Parameter resources were found via lilv, fall back to parsing TTL files in the bundle directory
    if (resourceParamsFound == 0) {
        const LilvNode* lib_node = lilv_plugin_get_library_uri(plugin);
        if (lib_node && lilv_node_is_uri(lib_node)) {
            const char* liburi = lilv_node_as_uri(lib_node);
            const char* prefix = "file://";
            const char* path = liburi;
            if (strncmp(liburi, prefix, strlen(prefix)) == 0) path = liburi + strlen(prefix);
            // derive bundle dir by removing filename
            std::string bundlePath(path);
            size_t slash = bundlePath.rfind('/');
            if (slash != std::string::npos) {
                std::string bundleDir = bundlePath.substr(0, slash + 1);
                // look for common ttl files
                const char* candidates[] = {"dsp.ttl","manifest.ttl","ui.ttl",NULL};
                for (int ci=0; candidates[ci]; ++ci) {
                    std::string f = bundleDir + candidates[ci];
                    I_File* fh = FS_FOPEN(const_cast<char*>(f.c_str()), const_cast<char*>("r"));
                    if (!fh) continue;
                    // read full file into a string
                    std::string contents;
                    const int BUF_SZ = 4096;
                    char buf[BUF_SZ];
                    int r = 0;
                    while ((r = fh->Read(buf, 1, BUF_SZ)) > 0) {
                        contents.append(buf, r);
                    }
                    fh->Close();

                    // crude TTL parse: find 'a lv2:Parameter' blocks by splitting on blank lines
                    size_t pos = 0;
                    while (pos < contents.size()) {
                        size_t next = contents.find("\n\n", pos);
                        std::string block;
                        if (next == std::string::npos) { block = contents.substr(pos); pos = contents.size(); }
                        else { block = contents.substr(pos, next - pos); pos = next + 2; }

                        if (block.find("a lv2:Parameter") == std::string::npos) continue;

                        // find subject token at start of block (first token before whitespace)
                        size_t first_nl = block.find('\n');
                        std::string firstline = (first_nl == std::string::npos) ? block : block.substr(0, first_nl);
                        std::string subject;
                        size_t sp = firstline.find(' ');
                        if (sp != std::string::npos) subject = firstline.substr(0, sp); else subject = firstline;

                        std::string pname = subject;
                        size_t col = pname.rfind(':'); if (col != std::string::npos) pname = pname.substr(col+1);

                        // skip if already present
                        bool exists=false; for (size_t pi=0; pi<parameters_.size(); ++pi) if (parameters_[pi].name==pname) { exists=true; break; }
                        if (exists) continue;

                        LV2PluginParameter param;
                        param.name = pname;
                        param.groupName = "";
                        param.minValue = 0.0f;
                        param.maxValue = 1.0f;
                        param.defaultValue = 0.0f;

                        // label
                        size_t p = block.find("rdfs:label");
                        if (p!=std::string::npos) {
                            size_t q = block.find('"', p);
                            if (q!=std::string::npos) {
                                size_t r2 = block.find('"', q+1);
                                if (r2!=std::string::npos) param.name = block.substr(q+1, r2-q-1);
                            }
                        }
                        // min/max/default
                        p = block.find("lv2:minimum"); if (p!=std::string::npos) { float v=0; sscanf(block.c_str()+p, "lv2:minimum %f", &v); param.minValue=v; }
                        p = block.find("lv2:maximum"); if (p!=std::string::npos) { float v=1; sscanf(block.c_str()+p, "lv2:maximum %f", &v); param.maxValue=v; }
                        p = block.find("lv2:default"); if (p!=std::string::npos) { float v=0; sscanf(block.c_str()+p, "lv2:default %f", &v); param.defaultValue=v; }

                        param.currentValue = param.defaultValue;
                        param.portIndex = -1;
                        param.variable = nullptr;

                        // create variable
                        std::string varDisplay = (param.groupName.empty() ? param.name : (param.groupName + ":" + param.name));
                        char varName[64]; if (varDisplay.length()>20) { std::string shortName = varDisplay.substr(0,17)+"..."; snprintf(varName,64, "%s", shortName.c_str()); } else { snprintf(varName,64, "%s", varDisplay.c_str()); }
                        int scaledValue = (int)(((param.currentValue - param.minValue) / (param.maxValue - param.minValue) * 127.0f) + 0.5f);
                        if (scaledValue<0) {
                            scaledValue = 0;
                        }
                        if (scaledValue > 127) {
                            scaledValue = 127;
                        }
                        WatchedVariable *wv2 = new WatchedVariable(varName, MAKE_FOURCC('L','P', (int)parameters_.size()/256, (int)parameters_.size()%256), scaledValue);
                        Insert(wv2);
                        param.variable = (Variable*)wv2;
                        wv2->AddObserver(*this);
                        parameters_.push_back(param);
                        controlValues_.push_back(param.defaultValue);
                        resourceParamsFound++;
                    }
                }
            }
        }
    }
    lilv_node_free(param_type);

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

    // Push initial parameter values into connected ports/atom buffers so plugins receive default state
    for (size_t pi = 0; pi < parameters_.size(); ++pi) {
        float v = parameters_[pi].currentValue;
        SetParameterValue((int)pi, v);
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
                    Trace::Debug("LV2Instrument: Connected audio output L port %u", i);
                } else if ((int)i == audioOutputPortR_ && audioBufferR_) {
                    lilv_instance_connect_port(static_cast<LilvInstance*>(pluginInstance_), i, audioBufferR_);
                    Trace::Debug("LV2Instrument: Connected audio output R port %u", i);
                } else {
                    // Connect to audioBufferL_ by default for additional channels
                    lilv_instance_connect_port(static_cast<LilvInstance*>(pluginInstance_), i, audioBufferL_ ? audioBufferL_ : (void*)nullptr);
                    Trace::Debug("LV2Instrument: Connected audio output (fallback) port %u", i);
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
            Trace::Debug("LV2Instrument: Connecting atom port %u", i);

            // MIDI input port uses preallocated midiBuffer_
            if ((int)i == midiInputPort_ && midiBuffer_) {
                lilv_instance_connect_port(static_cast<LilvInstance*>(pluginInstance_), i, midiBuffer_);
                Trace::Debug("LV2Instrument: Connected MIDI input port %u", i);
                continue;
            }

            // Allocate an atom input buffer sized to midiBufferSize_ (safe upper bound)
            size_t bufSize = (midiBufferSize_ > 0) ? midiBufferSize_ : 8192;
            if (atomInputBuffers_[i]) {
                delete[] atomInputBuffers_[i];
                atomInputBuffers_[i] = nullptr;
                atomInputBufferSizes_[i] = 0;
            }
            atomInputBuffers_[i] = new uint8_t[bufSize];
            memset(atomInputBuffers_[i], 0, bufSize);
            atomInputBufferSizes_[i] = bufSize;
            lilv_instance_connect_port(static_cast<LilvInstance*>(pluginInstance_), i, atomInputBuffers_[i]);
            Trace::Debug("LV2Instrument: Allocated and connected atom buffer for port %u size=%zu", i, bufSize);
            continue;
        }

        Trace::Debug("LV2Instrument: Port %u is unclassified; no connection made", i);
    }

    // Free temporary nodes
    lilv_node_free(audio_class);
    lilv_node_free(control_class);
    lilv_node_free(input_class);
    lilv_node_free(output_class);
    lilv_node_free(atom_class);

}

void LV2Instrument::SetParameterValue(int paramIndex, float value) {
    Trace::Debug("LV2Instrument: SetParameterValue called paramIndex=%d value=%g", paramIndex, value);
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

            Trace::Debug("LV2Instrument: SetParameterValue paramIndex=%d name=%s portIndex=%d isAtom=%d isOutput=%d target=%p audioL=%p audioR=%p audioInL=%p audioInR=%p",
                         paramIndex, parameters_[paramIndex].name.c_str(), pidx, parameters_[paramIndex].isAtomPort ? 1 : 0, parameters_[paramIndex].isOutput ? 1 : 0,
                         (void*)target, (void*)audioBufferL_, (void*)audioBufferR_, (void*)audioInputL_, (void*)audioInputR_, bsz);

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
                // Persistent diagnostic: write to /tmp for post-mortem inspection
                {
                    std::ofstream ofs("/tmp/lv2_setparam.log", std::ios::app);
                    if (ofs) {
                        ofs << "SKIP param=" << paramIndex << " name=" << parameters_[paramIndex].name << " port=" << pidx << " value=" << value
                            << " target=" << (void*)target << " audioL=" << (void*)audioBufferL_ << " audioR=" << (void*)audioBufferR_
                            << " inL=" << (void*)audioInputL_ << " inR=" << (void*)audioInputR_ << " bufSize=" << bsz << "\n";
                    }
                }
            } else {
                portControlStorage_[pidx] = value;
                Trace::Debug("LV2Instrument: SetParameterValue wrote float %g to portIndex=%d (isAtom=%d) storage=%p", value, pidx, parameters_[paramIndex].isAtomPort ? 1 : 0, (void*)&portControlStorage_[pidx]);
                // Persistent diagnostic: write successful writes to /tmp
                {
                    std::ofstream ofs("/tmp/lv2_setparam.log", std::ios::app);
                    if (ofs) {
                        ofs << "WRITE param=" << paramIndex << " name=" << parameters_[paramIndex].name << " port=" << pidx << " value=" << value << " storage=" << (void*)&portControlStorage_[pidx] << "\n";
                    }
                }
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
                Trace::Debug("LV2Instrument: cannot send atom patch for atom-control paramIndex=%d - no resource URI", (int)paramIndex);
            }
        } else {
            // No direct control port - try to send a patch:Set message if we have a resource URI and an atom input port
            if (!parameters_[paramIndex].resourceURI.empty() && midiInputPort_ >= 0 && midiBuffer_) {
                // Best-effort: send a patch:Set message describing subject + value using LV2_Atom_Forge
                LV2_Atom_Forge forge;
                lv2_atom_forge_init(&forge, &g_uridMapFeature);

                const size_t TMP_SIZE = 1024;
                uint8_t tmp[TMP_SIZE];
                lv2_atom_forge_set_buffer(&forge, tmp, TMP_SIZE);

                LV2_Atom_Forge_Frame frame;
                // Create a typed object of type patch:Set
                lv2_atom_forge_object(&forge, &frame, 0, urid_map(nullptr, LV2_PATCH__Set));
                // subject
                lv2_atom_forge_key(&forge, urid_map(nullptr, LV2_PATCH__subject));
                lv2_atom_forge_uri(&forge, parameters_[paramIndex].resourceURI.c_str(), (uint32_t)parameters_[paramIndex].resourceURI.length());
                // value
                lv2_atom_forge_key(&forge, urid_map(nullptr, LV2_PATCH__value));
                lv2_atom_forge_float(&forge, value);
                lv2_atom_forge_pop(&forge, &frame);

                // The forge wrote a full atom (header + body) into tmp with total length forge.offset
                if (forge.offset >= sizeof(LV2_Atom)) {
                    LV2_Atom* atom = (LV2_Atom*)tmp;
                    uint32_t bodySize = atom->size; // size of inner atom body
                    uint32_t atomType = atom->type; // URID

                    // Dump the forged atom bytes for offline analysis (helpful when plugins don't accept our format)
                    {
                        static int dumpCounter = 0;
                        ++dumpCounter;
                        std::string dumpPath = std::string("/tmp/lv2_patchset_forge_") + std::to_string(getpid()) + "_" + std::to_string(dumpCounter) + ".bin";
                        std::ofstream ofs(dumpPath, std::ios::binary);
                        if (ofs) {
                            ofs.write((char*)tmp, forge.offset);
                            Trace::Debug("LV2Instrument: dumped forged patch:Set atom to %s (size=%u)", dumpPath.c_str(), (unsigned)forge.offset);
                        }
                    }

                    // Primary variant: use the forged object's body with its typed atom type (likely LV2_PATCH__Set)
                    PendingAtomEvent ae;
                    ae.type = atomType; // use the typed atom type (should be LV2_PATCH__Set)
                    ae.data.resize(bodySize);
                    memcpy(ae.data.data(), LV2_ATOM_BODY(atom), bodySize);
                    // Default destination: MIDI atom input port
                    ae.destPortIndex = midiInputPort_ >= 0 ? midiInputPort_ : -1;
                    // If this parameter is associated with an atom-designated control port, target that port instead
                    if (parameters_[paramIndex].portIndex >= 0 && parameters_[paramIndex].isAtomPort) {
                        ae.destPortIndex = parameters_[paramIndex].portIndex;
                    }

                    Trace::Debug("LV2Instrument: queued patch:Set atom type=%u bodySize=%u for paramIndex=%d paramURI=%s destPort=%d", ae.type, (unsigned)ae.data.size(), (int)paramIndex, parameters_[paramIndex].resourceURI.c_str(), ae.destPortIndex);
                    pendingAtomEvents_.push_back(std::move(ae));

                    // Fallback variant 1: some plugins expect the event body typed as LV2_ATOM__Object rather than the patch:Set URID.
                    PendingAtomEvent aeObj;
                    aeObj.type = urid_map(nullptr, LV2_ATOM__Object);
                    aeObj.data.resize(bodySize);
                    memcpy(aeObj.data.data(), LV2_ATOM_BODY(atom), bodySize);
                    aeObj.destPortIndex = midiInputPort_ >= 0 ? midiInputPort_ : -1;
                    if (parameters_[paramIndex].portIndex >= 0 && parameters_[paramIndex].isAtomPort) {
                        aeObj.destPortIndex = parameters_[paramIndex].portIndex;
                    }
                    Trace::Debug("LV2Instrument: queued fallback object-typed patch:Set for paramIndex=%d destPort=%d", (int)paramIndex, aeObj.destPortIndex);
                    pendingAtomEvents_.push_back(std::move(aeObj));

                    // Fallback variant 2: forge an LV2_ATOM__Object that contains an explicit rdf:type = LV2_PATCH__Set,
                    // plus subject & value keys — some plugins expect rdf:type-based objects rather than typed atoms.
                    {
                        LV2_Atom_Forge of;
                        lv2_atom_forge_init(&of, &g_uridMapFeature);
                        uint8_t otmp[256]; lv2_atom_forge_set_buffer(&of, otmp, sizeof(otmp));

                        LV2_Atom_Forge_Frame oframe;
                        lv2_atom_forge_object(&of, &oframe, 0, urid_map(nullptr, LV2_ATOM__Object));
                        // rdf:type => LV2_PATCH__Set
                        lv2_atom_forge_key(&of, urid_map(nullptr, LV2_RDF__type));
                        lv2_atom_forge_urid(&of, urid_map(nullptr, LV2_PATCH__Set));
                        // subject
                        lv2_atom_forge_key(&of, urid_map(nullptr, LV2_PATCH__subject));
                        lv2_atom_forge_uri(&of, parameters_[paramIndex].resourceURI.c_str(), (uint32_t)parameters_[paramIndex].resourceURI.length());
                        // value
                        lv2_atom_forge_key(&of, urid_map(nullptr, LV2_PATCH__value));
                        lv2_atom_forge_float(&of, value);
                        lv2_atom_forge_pop(&of, &oframe);

                        if (of.offset >= sizeof(LV2_Atom)) {
                            // Queue the URI-subject object as before

                            LV2_Atom* oatom = (LV2_Atom*)otmp;
                            uint32_t obody = oatom->size;
                            PendingAtomEvent aeObj2;
                            aeObj2.type = urid_map(nullptr, LV2_ATOM__Object);
                            aeObj2.data.resize(obody);
                            memcpy(aeObj2.data.data(), LV2_ATOM_BODY(oatom), obody);
                            aeObj2.destPortIndex = midiInputPort_ >= 0 ? midiInputPort_ : -1;
                            if (parameters_[paramIndex].portIndex >= 0 && parameters_[paramIndex].isAtomPort) {
                                aeObj2.destPortIndex = parameters_[paramIndex].portIndex;
                            }
                            Trace::Debug("LV2Instrument: queued rdf-typed object patch:Set (subject=URI) for paramIndex=%d destPort=%d", (int)paramIndex, aeObj2.destPortIndex);
                            pendingAtomEvents_.push_back(std::move(aeObj2));

                            // Additional fallback: encode subject as URID instead of URI
                            LV2_Atom_Forge of2;
                            lv2_atom_forge_init(&of2, &g_uridMapFeature);
                            uint8_t otmp2[256]; lv2_atom_forge_set_buffer(&of2, otmp2, sizeof(otmp2));
                            LV2_Atom_Forge_Frame of2frame;
                            lv2_atom_forge_object(&of2, &of2frame, 0, urid_map(nullptr, LV2_ATOM__Object));
                            lv2_atom_forge_key(&of2, urid_map(nullptr, LV2_RDF__type));
                            lv2_atom_forge_urid(&of2, urid_map(nullptr, LV2_PATCH__Set));
                            lv2_atom_forge_key(&of2, urid_map(nullptr, LV2_PATCH__subject));
                            // Encode subject as URID now
                            lv2_atom_forge_urid(&of2, urid_map(nullptr, parameters_[paramIndex].resourceURI.c_str()));
                            lv2_atom_forge_key(&of2, urid_map(nullptr, LV2_PATCH__value));
                            lv2_atom_forge_float(&of2, value);
                            lv2_atom_forge_pop(&of2, &of2frame);

                            if (of2.offset >= sizeof(LV2_Atom)) {
                                LV2_Atom* oatom2 = (LV2_Atom*)otmp2;
                                uint32_t obody2 = oatom2->size;
                                PendingAtomEvent aeObj3;
                                aeObj3.type = urid_map(nullptr, LV2_ATOM__Object);
                                aeObj3.data.resize(obody2);
                                memcpy(aeObj3.data.data(), LV2_ATOM_BODY(oatom2), obody2);
                                aeObj3.destPortIndex = midiInputPort_ >= 0 ? midiInputPort_ : -1;
                                if (parameters_[paramIndex].portIndex >= 0 && parameters_[paramIndex].isAtomPort) {
                                    aeObj3.destPortIndex = parameters_[paramIndex].portIndex;
                                }
                                Trace::Debug("LV2Instrument: queued rdf-typed object patch:Set (subject=URID) for paramIndex=%d destPort=%d", (int)paramIndex, aeObj3.destPortIndex);
                                pendingAtomEvents_.push_back(std::move(aeObj3));
                            }
                        }

                        // Fallback variant 3: wrap the forged LV2_Atom as an LV2_ATOM__Atom payload (some hosts nest atoms)
                        PendingAtomEvent aeWrapped;
                        aeWrapped.type = urid_map(nullptr, LV2_ATOM__Atom);
                        aeWrapped.data.resize(forge.offset);
                        memcpy(aeWrapped.data.data(), tmp, forge.offset);
                        aeWrapped.destPortIndex = midiInputPort_ >= 0 ? midiInputPort_ : -1;
                        if (parameters_[paramIndex].portIndex >= 0 && parameters_[paramIndex].isAtomPort) {
                            aeWrapped.destPortIndex = parameters_[paramIndex].portIndex;
                        }
                        Trace::Debug("LV2Instrument: queued wrapped-atom patch:Set (type=Atom) for paramIndex=%d destPort=%d", (int)paramIndex, aeWrapped.destPortIndex);
                        pendingAtomEvents_.push_back(std::move(aeWrapped));

                    }
                } else {
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

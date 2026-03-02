#include "LV2Instrument.h"
#include "CommandList.h"
#include "System/Console/Trace.h"
#include "Application/Mixer/MixerService.h"
#include "Services/Audio/Audio.h"
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
#include <lv2/state/state.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fstream>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "Foundation/Variables/WatchedVariable.h"
#include "OsTIrusPatches.h"

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
// Actual render size is tempo-dependent (typically ~460–920 samples at 120 BPM / 44100 Hz).
static uint32_t s_minBlock = 1u;
static uint32_t s_nominalBlock = 512u;
static uint32_t s_maxBlock = 8192u;
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
    portsConnected_ = false;
    forcedOutputChannels_ = 0; // default to automatic
    cachedOutputBuffer_ = nullptr;
    cachedOutputSize_ = 0;
    renderChannelMask_ = 0;
    cachedVolVar_ = nullptr;

    // Preset state
    currentBank_ = 0;
    currentPreset_ = 0;
    usingFilePresets_ = false;
    usingMidiPresets_ = false;
    userCcToParamIdx_.clear();
    programChangeEnabled_ = false;

    // Initialize options array entries so plugins can read block-size options
    init_options_array();

    // Initialize channel state
    // Compute reverb buffer length based on actual sample rate (~100ms)
    int sampleRate = Audio::GetInstance() ? Audio::GetInstance()->GetSampleRate() : 44100;
    if (sampleRate <= 0) sampleRate = 44100;
    reverbBufferLength_ = (sampleRate / 10); // ~100ms worth of samples
    if (reverbBufferLength_ < 1024) reverbBufferLength_ = 1024;

    // Scale tap offsets to actual sample rate (original values tuned for 44100 Hz)
    float rateRatio = (float)sampleRate / 44100.0f;
    reverbTapOffsets_[0] = (int)(743.0f * rateRatio);
    reverbTapOffsets_[1] = (int)(1557.0f * rateRatio);
    reverbTapOffsets_[2] = (int)(2311.0f * rateRatio);
    reverbTapOffsets_[3] = (int)(2801.0f * rateRatio);
    reverbTapOffsets_[4] = (int)(3571.0f * rateRatio);
    reverbTapOffsets_[5] = (int)(4409.0f * rateRatio);
    // Clamp tap offsets to buffer length
    for (int t = 0; t < 6; t++) {
        if (reverbTapOffsets_[t] >= reverbBufferLength_) {
            reverbTapOffsets_[t] = reverbBufferLength_ - 1;
        }
    }

    for (int i = 0; i < SONG_CHANNEL_COUNT; i++) {
        lastNote_[i] = -1;
        playing_[i] = false;
        // Allocate and initialize reverb state
        filterLFO_[i]  = LV2LFOState();
        arpState_[i]   = LV2ArpState();
        crushBits_[i]  = 16;
        crushDrive_[i] = 0;
        reverbBuffer_[i] = new fixed[reverbBufferLength_ * 2];
        memset(reverbBuffer_[i], 0, reverbBufferLength_ * 2 * sizeof(fixed));
        reverbDecay_[i] = 0;
        reverbSend_[i] = 0;
        reverbDamp_[i] = 0;
        reverbDampL_[i] = 0;
        reverbDampR_[i] = 0;
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
    // Free dynamically allocated reverb buffers
    for (int i = 0; i < SONG_CHANNEL_COUNT; i++) {
        if (reverbBuffer_[i]) {
            delete[] reverbBuffer_[i];
            reverbBuffer_[i] = nullptr;
        }
    }
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

    // Extract saved bank/preset before clearing (these Variables
    // are only created by the view, so FindVariable won't find
    // them — we must restore them explicitly)
    int savedBank = -1, savedPreset = -1;
    auto bankIt = pendingParamValues_.find("bank");
    if (bankIt != pendingParamValues_.end())
        savedBank = atoi(bankIt->second.c_str());
    auto presetIt = pendingParamValues_.find("preset");
    if (presetIt != pendingParamValues_.end())
        savedPreset = atoi(presetIt->second.c_str());

    pendingParamValues_.clear();

    // Restore bank/preset selection after state is loaded
    if (savedBank >= 0)
        SetCurrentBank(savedBank);
    if (savedPreset >= 0)
        SetPreset(savedPreset);

    return true;
}

void LV2Instrument::OnStart() {
    tableState_.Reset();
}

bool LV2Instrument::Start(int channel, unsigned char note, bool retrigger) {
    if (IsEmpty()) {
        return false;
    }

    if (channel < 0 || channel >= SONG_CHANNEL_COUNT) {
        return false;
    }

    // Always use MIDI channel 0 for all tracker channels.  Many LV2 synths
    // (e.g. Mutated) are monophonic and ignore the MIDI channel byte
    // (lv2_midi_message_type does msg[0]&0xF0).  Using different channels
    // per tracker channel causes note-off mismatches: the plugin only
    // releases when msg[1]==current_note, and a second channel's note-on
    // overwrites current_note, making the first channel's note-off fail.
    // The result is stuck notes / unreleased envelopes → static.

    {
        SysMutexLocker lock(pendingEventsMutex_);

        // Stop any existing note on this channel first
        if (playing_[channel] && lastNote_[channel] >= 0) {
            MidiEvent noteOff;
            noteOff.data[0] = 0x80;  // Note Off on MIDI ch 0
            noteOff.data[1] = lastNote_[channel];
            noteOff.data[2] = 0;
            noteOff.size = 3;
            pendingMidiEvents_.push_back(noteOff);
        }

        // Queue MIDI note-on event
        MidiEvent noteOn;
        noteOn.data[0] = 0x90;  // Note On on MIDI ch 0
        noteOn.data[1] = note;
        noteOn.data[2] = nextVelocity_;   // Velocity from MIDI input or 127
        noteOn.size = 3;
        pendingMidiEvents_.push_back(noteOn);
        nextVelocity_ = 127; // reset for tracker playback

        lastNote_[channel] = note;
        playing_[channel] = true;
    }

    // Reset LFO modulation on new note
    tremoloLFO_[channel].active = false;
    tremoloLFO_[channel].phase = 0;
    vibratoLFO_[channel].active = false;
    vibratoLFO_[channel].phase = 0;
    filterLFO_[channel].active  = false;
    filterLFO_[channel].phase   = 0;
    // Reset arpeggio so next ARPG command starts a fresh cycle
    arpState_[channel].active   = false;
    arpState_[channel].position = 0;
    arpState_[channel].baseNote = note;
    // Reset bit-crush to bypass
    crushBits_[channel]  = 16;
    crushDrive_[channel] = 0;

    return true;
}

void LV2Instrument::Stop(int channel) {
    if (channel < 0 || channel >= SONG_CHANNEL_COUNT) {
        return;
    }

    {
        SysMutexLocker lock(pendingEventsMutex_);

        if (!playing_[channel]) {
            return;
        }

        playing_[channel] = false;

        // Queue MIDI note-off event (always MIDI channel 0 — see Start() comment)
        if (lastNote_[channel] >= 0) {
            MidiEvent noteOff;
            noteOff.data[0] = 0x80;  // Note Off on MIDI ch 0
            noteOff.data[1] = lastNote_[channel];
            noteOff.data[2] = 0;
            noteOff.size = 3;
            pendingMidiEvents_.push_back(noteOff);
        }
    }
}

bool LV2Instrument::Render(int channel, fixed *buffer, int size, bool updateTick) {
    
    if (channel < 0 || channel >= SONG_CHANNEL_COUNT) {
        memset(buffer, 0, size * 2 * sizeof(fixed));
        return false;
    }

    if (IsEmpty()) {
        memset(buffer, 0, size * 2 * sizeof(fixed));
        return false;
    }

    bool noteActive = playing_[channel];

    if (!pluginInstance_) {
        memset(buffer, 0, size * 2 * sizeof(fixed));
        return noteActive;
    }

    // Detect new audio cycle: if this channel was already rendered, a new
    // cycle has begun.  Reset the mask and force a fresh plugin run.
    uint32_t channelBit = (1u << channel);
    if (renderChannelMask_ & channelBit) {
        renderChannelMask_ = 0;  // new cycle
    }

    bool needsPluginRun = (renderChannelMask_ == 0);
    renderChannelMask_ |= channelBit;

    // ---- Run plugin ONCE per audio cycle (first channel to render) ----
    if (needsPluginRun) {

        // Allocate buffers if needed
        const int BUFFER_SIZE = 2048;
        if (!audioBufferL_) {
            audioBufferL_ = new float[BUFFER_SIZE];
            audioBufferR_ = new float[BUFFER_SIZE];
            audioInputL_ = new float[BUFFER_SIZE];
            audioInputR_ = new float[BUFFER_SIZE];
            audioDummyBuffer_ = new float[BUFFER_SIZE];
            bufferSize_ = BUFFER_SIZE;
            memset(audioBufferL_, 0, BUFFER_SIZE * sizeof(float));
            memset(audioBufferR_, 0, BUFFER_SIZE * sizeof(float));
            memset(audioInputL_, 0, BUFFER_SIZE * sizeof(float));
            memset(audioInputR_, 0, BUFFER_SIZE * sizeof(float));
            memset(audioDummyBuffer_, 0, BUFFER_SIZE * sizeof(float));
            pendingAtomEvents_.reserve(128);
            renderLocalMidi_.reserve(32);
            renderLocalAtom_.reserve(128);
            renderPatchEvents_.reserve(64);
        }

        // Grow buffers if needed
        if (size > bufferSize_) {
            
            delete[] audioBufferL_;
            delete[] audioBufferR_;
            delete[] audioInputL_;
            delete[] audioInputR_;
            delete[] audioDummyBuffer_;
            bufferSize_ = size;
            audioBufferL_ = new float[bufferSize_];
            audioBufferR_ = new float[bufferSize_];
            audioInputL_ = new float[bufferSize_];
            audioInputR_ = new float[bufferSize_];
            audioDummyBuffer_ = new float[bufferSize_];
            connectPorts(bufferSize_);
        }

        // Ensure cached output buffer is large enough
        if (!cachedOutputBuffer_ || cachedOutputSize_ < size) {
            delete[] cachedOutputBuffer_;
            cachedOutputBuffer_ = new fixed[size * 2];
            cachedOutputSize_ = size;
        }

        // Connect ports on first render
        if (!portsConnected_ && audioBufferL_) {
            connectPorts(bufferSize_);
            portsConnected_ = true;
        }

        // Activate plugin once after first connection
        if (!isActivated_ && pluginInstance_) {
            LilvInstance* instance = (LilvInstance*)pluginInstance_;
            lilv_instance_activate(instance);
            isActivated_ = true;
            for (size_t pi = 0; pi < parameters_.size(); ++pi) {
                int pidx = parameters_[pi].portIndex;
                if (pidx >= 0 && !parameters_[pi].isAtomPort && !parameters_[pi].isOutput) {
                    if (pidx < (int)portControlStorage_.size()) {
                        portControlStorage_[pidx] = parameters_[pi].currentValue;
                    }
                }
            }
        }

        // Clear input buffers
        memset(audioInputL_, 0, size * sizeof(float));
        memset(audioInputR_, 0, size * sizeof(float));

        // Update parameter values from Variables.
        // We write directly to portControlStorage_ here (audio thread only)
        // instead of calling SetParameterValue() to avoid its overhead and
        // ensure the audio thread is the sole writer to port storage.
        renderPatchEvents_.clear();
        for (size_t i = 0; i < parameters_.size() && i < controlValues_.size(); i++) {
            if (parameters_[i].isOutput) continue;
            if (!parameters_[i].variable) continue;

            int scaledValue = parameters_[i].variable->GetInt();
            float realValue = parameters_[i].minValue +
                (scaledValue / 127.0f) * (parameters_[i].maxValue - parameters_[i].minValue);

            // Snap to nearest scale point if available
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
                float rounded = std::round(realValue);
                if (rounded < parameters_[i].minValue) rounded = parameters_[i].minValue;
                if (rounded > parameters_[i].maxValue) rounded = parameters_[i].maxValue;
                realValue = rounded;
            }

            // Only update if value actually changed
            if (std::fabs(realValue - parameters_[i].currentValue) < 1e-6f) {
                continue;
            }

            parameters_[i].currentValue = realValue;
            controlValues_[i] = realValue;

            int pidx = parameters_[i].portIndex;
            if (pidx >= 0 && !parameters_[i].isAtomPort) {
                // Standard control port: write directly to port storage
                if (pidx < (int)portControlStorage_.size()) {
                    portControlStorage_[pidx] = realValue;
                }
            } else if (pidx < 0 && !parameters_[i].resourceURI.empty()
                       && midiInputPort_ >= 0 && midiBuffer_) {
                // Resource-backed param: queue a patch:Set atom event.
                // These will be written to the atom buffer below alongside
                // any pending events from Start/Stop.
                LV2_Atom_Forge forge;
                lv2_atom_forge_init(&forge, &g_uridMapFeature);
                const size_t TMP_SIZE = 1024;
                uint8_t tmp[TMP_SIZE];
                lv2_atom_forge_set_buffer(&forge, tmp, TMP_SIZE);
                LV2_Atom_Forge_Frame frame;
                lv2_atom_forge_object(&forge, &frame, 0,
                    urid_map(nullptr, LV2_PATCH__Set));
                lv2_atom_forge_key(&forge,
                    urid_map(nullptr, LV2_PATCH__property));
                lv2_atom_forge_urid(&forge,
                    urid_map(nullptr, parameters_[i].resourceURI.c_str()));
                lv2_atom_forge_key(&forge,
                    urid_map(nullptr, LV2_PATCH__value));
                lv2_atom_forge_float(&forge, realValue);
                lv2_atom_forge_pop(&forge, &frame);

                if (forge.offset >= sizeof(LV2_Atom)) {
                    LV2_Atom* atom = (LV2_Atom*)tmp;
                    PendingAtomEvent ae;
                    ae.type = atom->type;
                    ae.data.resize(atom->size);
                    memcpy(ae.data.data(), LV2_ATOM_BODY(atom), atom->size);
                    ae.destPortIndex = midiInputPort_;
                    // These go directly into the local list that will be
                    // written to the atom buffer below — no mutex needed
                    // since we are on the audio thread.
                    renderPatchEvents_.push_back(std::move(ae));
                }
            }
        }

        // ---- Clear ALL atom input buffers to empty sequences FIRST ----
        // This prevents stale events from the previous cycle being
        // re-read by the plugin (the root cause of static/oscillation).
        if (midiBuffer_ && midiInputPort_ >= 0) {
            LV2_Atom_Sequence* seq = (LV2_Atom_Sequence*)midiBuffer_;
            seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
            seq->atom.type = g_atomSequenceUrid;
            seq->body.unit = 0;
            seq->body.pad = 0;
        }
        for (size_t ai = 0; ai < atomInputBuffers_.size(); ++ai) {
            if (atomInputBuffers_[ai]) {
                LV2_Atom_Sequence* seq = (LV2_Atom_Sequence*)atomInputBuffers_[ai];
                seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
                seq->atom.type = g_atomSequenceUrid;
                seq->body.unit = 0;
                seq->body.pad = 0;
            }
        }

        // ---- Drain pending events under lock ----
        // Use Lock() instead of TryLock() to guarantee we never silently
        // drop pending note events — losing a note-off causes stuck notes.
        renderLocalMidi_.clear();
        renderLocalAtom_.clear();
        pendingEventsMutex_.Lock();
        renderLocalMidi_.swap(pendingMidiEvents_);
        renderLocalAtom_.swap(pendingAtomEvents_);
        pendingEventsMutex_.Unlock();

        // Append any patch:Set events generated by the param update loop above
        if (!renderPatchEvents_.empty()) {
            renderLocalAtom_.insert(renderLocalAtom_.end(),
                std::make_move_iterator(renderPatchEvents_.begin()),
                std::make_move_iterator(renderPatchEvents_.end()));
            renderPatchEvents_.clear();
        }

        // Write MIDI events to the atom/MIDI buffer
        if (midiBuffer_ && midiInputPort_ >= 0 && g_midiEventUrid > 0) {
            LV2_Atom_Sequence* seq = (LV2_Atom_Sequence*)midiBuffer_;
            uint8_t* buf = (uint8_t*)LV2_ATOM_CONTENTS(LV2_Atom_Sequence, seq);
            size_t offset = 0;
            size_t capacity = midiBufferSize_ - sizeof(LV2_Atom_Sequence);

            for (size_t i = 0; i < renderLocalMidi_.size() && offset + 32 < capacity; i++) {
                MidiEvent& evt = renderLocalMidi_[i];
                LV2_Atom_Event* event = (LV2_Atom_Event*)(buf + offset);
                event->time.frames = 0;
                event->body.size = evt.size;
                event->body.type = g_midiEventUrid;
                memcpy(LV2_ATOM_BODY(&event->body), evt.data, evt.size);
                size_t padded_size = sizeof(LV2_Atom_Event) + ((evt.size + 7) & ~7);
                offset += padded_size;
            }
            seq->atom.size = sizeof(LV2_Atom_Sequence_Body) + offset;
        }

        // Write pending atom events (patch:Set etc) to appropriate buffers
        if (!renderLocalAtom_.empty()) {
            auto appendEventsToBuffer = [&](uint8_t* buffer, size_t bufCap, const PendingAtomEvent &event) {
                if (!buffer) return;
                LV2_Atom_Sequence* seq = (LV2_Atom_Sequence*)buffer;
                size_t existingDataSize = seq->atom.size - sizeof(LV2_Atom_Sequence_Body);
                uint8_t* b = (uint8_t*)LV2_ATOM_CONTENTS(LV2_Atom_Sequence, seq);
                size_t off = existingDataSize;
                size_t cap = bufCap - sizeof(LV2_Atom_Sequence);
                size_t padded_size = sizeof(LV2_Atom_Event) + ((event.data.size() + 7) & ~7);
                if (off + padded_size > cap) return;
                LV2_Atom_Event* ev = (LV2_Atom_Event*)(b + off);
                ev->time.frames = 0;
                ev->body.size = (uint32_t)event.data.size();
                ev->body.type = event.type;
                memcpy(LV2_ATOM_BODY(&ev->body), event.data.data(), event.data.size());
                off += padded_size;
                seq->atom.size = sizeof(LV2_Atom_Sequence_Body) + off;
            };

            for (auto &ae : renderLocalAtom_) {
                int dest = ae.destPortIndex;
                if (dest < 0) {
                    if (midiBuffer_) {
                        appendEventsToBuffer(midiBuffer_, midiBufferSize_, ae);
                    }
                    for (int port = 0; port < (int)atomInputBuffers_.size(); ++port) {
                        if (atomInputBuffers_[port]) {
                            appendEventsToBuffer(atomInputBuffers_[port], atomInputBufferSizes_[port], ae);
                        }
                    }
                } else if (dest == midiInputPort_ && midiBuffer_) {
                    appendEventsToBuffer(midiBuffer_, midiBufferSize_, ae);
                } else if (dest >= 0 && dest < (int)atomInputBuffers_.size() && atomInputBuffers_[dest]) {
                    appendEventsToBuffer(atomInputBuffers_[dest], atomInputBufferSizes_[dest], ae);
                }
            }
        }

        // Re-initialize atom OUTPUT buffers as empty sequences
        for (size_t oi = 0; oi < atomOutputBuffers_.size(); ++oi) {
            if (atomOutputBuffers_[oi]) {
                LV2_Atom_Sequence* seq = (LV2_Atom_Sequence*)atomOutputBuffers_[oi];
                seq->atom.type = g_atomSequenceUrid;
                seq->atom.size = atomOutputBufferSizes_[oi] - sizeof(LV2_Atom);
                seq->body.unit = 0;
                seq->body.pad = 0;
            }
        }

        // Run the plugin
        LilvInstance* instance = (LilvInstance*)pluginInstance_;
        memset(audioBufferL_, 0, size * sizeof(float));
        memset(audioBufferR_, 0, size * sizeof(float));
        memset(audioDummyBuffer_, 0, size * sizeof(float));

        // Some LV2 plugins (e.g. Mutated Instruments) process in even-sized
        // blocks internally (block_size &= ~1u).  When the host passes an odd
        // n_samples, the plugin's inner loop skips the last sample, leaving it
        // at 0 and creating a periodic 1-sample discontinuity that manifests
        // as high-frequency static/noise.  We run the plugin with an even
        // sample count and then fill the trailing sample by duplication.
        int runSize = size & ~1;  // round down to even
        if (runSize < 2) runSize = 2;  // minimum 2 samples
        if (runSize > size) runSize = size;  // safety: don't exceed buffer
        lilv_instance_run(instance, runSize);

        // Fill trailing sample if we rounded down
        if (runSize < size) {
            audioBufferL_[runSize] = audioBufferL_[runSize - 1];
            audioBufferR_[runSize] = audioBufferR_[runSize - 1];
        }

        // Sanitise plugin output: zero any NaN/Inf samples.
        // LV2 plugins with feedback networks (reverb, delay, resonant
        // filters) can produce NaN/Inf which would poison all downstream
        // processing.  We use a bitwise check that survives -ffast-math.
        {
            union { float f; uint32_t u; } chk;
            for (int i = 0; i < size; i++) {
                chk.f = audioBufferL_[i];
                if ((chk.u & 0x7F800000u) == 0x7F800000u) audioBufferL_[i] = 0.0f;
                chk.f = audioBufferR_[i];
                if ((chk.u & 0x7F800000u) == 0x7F800000u) audioBufferR_[i] = 0.0f;
            }
        }

        // If mono plugin (no right output port), duplicate left to right
        if (audioOutputPortR_ == -1 && audioBufferL_ && audioBufferR_) {
            memcpy(audioBufferR_, audioBufferL_, size * sizeof(float));
        }

        // Convert float output to fixed point stereo interleaved and cache.
        //
        // IMPORTANT: The host is compiled with -ffast-math which implies
        // -ffinite-math-only.  This means the compiler may assume NaN/Inf
        // never occur and can optimise away comparisons like (x > 1.0f)
        // when x is NaN (since NaN > 1.0f is false by IEEE 754, the
        // compiler removes the branch entirely).  LV2 plugins with
        // feedback networks (reverb, delay, resonant filters) CAN produce
        // NaN or Inf, so we must sanitise plugin output with a method the
        // compiler cannot remove.
        if (!cachedVolVar_) cachedVolVar_ = FindVariable(LV2IP_VOLUME);
        float volume = cachedVolVar_ ? (cachedVolVar_->GetInt() / 255.0f) : 1.0f;

        for (int i = 0; i < size; i++) {
            float l = audioBufferL_[i] * volume;
            float r = audioBufferR_[i] * volume;

            // Bitwise NaN/Inf check that survives -ffast-math.
            // IEEE 754: NaN/Inf have all exponent bits set (0x7F800000).
            union { float f; uint32_t u; } ul, ur;
            ul.f = l;
            ur.f = r;
            if ((ul.u & 0x7F800000u) == 0x7F800000u) l = 0.0f;
            if ((ur.u & 0x7F800000u) == 0x7F800000u) r = 0.0f;

            // Clamp to [-1, 1]
            if (l > 1.0f) l = 1.0f;
            if (l < -1.0f) l = -1.0f;
            if (r > 1.0f) r = 1.0f;
            if (r < -1.0f) r = -1.0f;
            cachedOutputBuffer_[i * 2] = i2fp((int)(l * 32767.0f));
            cachedOutputBuffer_[i * 2 + 1] = i2fp((int)(r * 32767.0f));
        }
    } // end needsPluginRun

    // Copy cached output to this channel's buffer (guard against stale/null cache)
    if (cachedOutputBuffer_ && cachedOutputSize_ >= size) {
        memcpy(buffer, cachedOutputBuffer_, size * 2 * sizeof(fixed));
    } else {
        memset(buffer, 0, size * 2 * sizeof(fixed));
    }

    // Apply per-channel tremolo modulation (volume LFO)
    if (tremoloLFO_[channel].active) {
        float phase = tremoloLFO_[channel].phase;
        float speed = tremoloLFO_[channel].speed;
        float depth = tremoloLFO_[channel].depth;
        for (int i = 0; i < size; i++) {
            float sine = sinf(phase * 2.0f * 3.14159265f / 256.0f);
            float volMul = 1.0f + sine * depth;
            if (volMul < 0.0f) volMul = 0.0f;
            fixed mul = fl2fp(volMul);
            fixed tl = fp_mul(buffer[i * 2], mul);
            fixed tr = fp_mul(buffer[i * 2 + 1], mul);
            if (tl > i2fp(32767)) tl = i2fp(32767);
            else if (tl < i2fp(-32768)) tl = i2fp(-32768);
            if (tr > i2fp(32767)) tr = i2fp(32767);
            else if (tr < i2fp(-32768)) tr = i2fp(-32768);
            buffer[i * 2]     = tl;
            buffer[i * 2 + 1] = tr;
            phase += speed;
            if (phase >= 256.0f) phase -= 256.0f;
        }
        tremoloLFO_[channel].phase = phase;
    }

    // Apply per-channel filter LFO (sends CC74 once per render block)
    if (filterLFO_[channel].active) {
        float sine = sinf(filterLFO_[channel].phase * 2.0f * 3.14159265f / 256.0f);
        int cc74 = 64 + (int)(sine * filterLFO_[channel].depth * 63.0f);
        if (cc74 < 0)   cc74 = 0;
        if (cc74 > 127) cc74 = 127;
        QueueMidiEvent(0xB0, 74, (unsigned char)cc74);
        filterLFO_[channel].phase += filterLFO_[channel].speed * (float)size;
        if (filterLFO_[channel].phase >= 256.0f) filterLFO_[channel].phase -= 256.0f;
    }

    // Apply per-channel reverb effect if enabled
    if (reverbSend_[channel] > 0 && reverbBuffer_[channel]) {
           fixed *reverbBuf = reverbBuffer_[channel];
           int reverbPos = reverbPos_[channel];
        fixed decay = reverbDecay_[channel];
        fixed send = reverbSend_[channel];
        fixed damp = reverbDamp_[channel];
        fixed dampInv = fp_sub(FP_ONE, damp);
        fixed dampStateL = reverbDampL_[channel];
        fixed dampStateR = reverbDampR_[channel];
        
        // 6 taps with cross-channel diffusion
        static const fixed tapGains[6] = {fl2fp(0.25f), fl2fp(0.30f), fl2fp(0.20f),
                                           fl2fp(0.18f), fl2fp(0.14f), fl2fp(0.10f)};

        // Feedback position offset scaled to sample rate
        const int fbSamples = (int)(100.0f * (float)reverbBufferLength_ / 4410.0f);
        
        fixed *outPtr = buffer;
        for (int i = 0; i < size; i++) {
            fixed dryL = *outPtr;
            fixed dryR = *(outPtr + 1);
            
            // Read from delay taps with cross-channel spread
            fixed wetL = 0;
            fixed wetR = 0;
            for (int t = 0; t < 6; t++) {
                    int readPos = reverbPos - reverbTapOffsets_[t];
                if (readPos < 0) readPos += reverbBufferLength_;
                
                fixed tapL = fp_mul(reverbBuf[readPos * 2], tapGains[t]);
                fixed tapR = fp_mul(reverbBuf[readPos * 2 + 1], tapGains[t]);
                if (t & 1) {
                    wetL = fp_add(wetL, tapR);
                    wetR = fp_add(wetR, tapL);
                } else {
                    wetL = fp_add(wetL, tapL);
                    wetR = fp_add(wetR, tapR);
                }
            }
            
            // Feedback with damping lowpass
            int fb_pos = reverbPos - reverbBufferLength_ + fbSamples;
            if (fb_pos < 0) fb_pos += reverbBufferLength_;
            fixed rawFbL = reverbBuf[fb_pos * 2];
            fixed rawFbR = reverbBuf[fb_pos * 2 + 1];
            
            dampStateL = fp_add(fp_mul(rawFbL, dampInv), fp_mul(dampStateL, damp));
            dampStateR = fp_add(fp_mul(rawFbR, dampInv), fp_mul(dampStateR, damp));
            fixed fbL = fp_mul(dampStateL, decay);
            fixed fbR = fp_mul(dampStateR, decay);

            fixed rbL = fp_add(fp_mul(dryL, send), fbL);
            fixed rbR = fp_add(fp_mul(dryR, send), fbR);
            if (rbL > i2fp(32767)) rbL = i2fp(32767);
            else if (rbL < i2fp(-32768)) rbL = i2fp(-32768);
            if (rbR > i2fp(32767)) rbR = i2fp(32767);
            else if (rbR < i2fp(-32768)) rbR = i2fp(-32768);
            reverbBuf[reverbPos * 2] = rbL;
            reverbBuf[reverbPos * 2 + 1] = rbR;
            
            // Mix wet with dry output (clamp to prevent fixed-point overflow)
            fixed mixL = fp_add(dryL, wetL);
            fixed mixR = fp_add(dryR, wetR);
            if (mixL > i2fp(32767)) mixL = i2fp(32767);
            if (mixL < i2fp(-32768)) mixL = i2fp(-32768);
            if (mixR > i2fp(32767)) mixR = i2fp(32767);
            if (mixR < i2fp(-32768)) mixR = i2fp(-32768);
            *outPtr = mixL;
            *(outPtr + 1) = mixR;
            
                outPtr += 2;
                reverbPos++;
                if (reverbPos >= reverbBufferLength_) {
                    reverbPos = 0;
                }
        }
        reverbPos_[channel] = reverbPos;
        reverbDampL_[channel] = dampStateL;
        reverbDampR_[channel] = dampStateR;
    }

    // Apply per-channel bit crush
    if (crushBits_[channel] < 16) {
        int shift = 16 - crushBits_[channel];
        fixed mask = (fixed)0xFFFFFFFF;
        if (shift != 0) mask <<= (FIXED_SHIFT + shift);
        fixed fpDrive = (crushDrive_[channel] > 0)
                        ? fl2fp(crushDrive_[channel] / 255.0f)
                        : FP_ONE;
        for (int i = 0; i < size; i++) {
            fixed l = fp_mul(buffer[i * 2],     fpDrive) & mask;
            fixed r = fp_mul(buffer[i * 2 + 1], fpDrive) & mask;
            if (l > i2fp(32767))       l = i2fp(32767);
            else if (l < i2fp(-32768)) l = i2fp(-32768);
            if (r > i2fp(32767))       r = i2fp(32767);
            else if (r < i2fp(-32768)) r = i2fp(-32768);
            buffer[i * 2]     = l;
            buffer[i * 2 + 1] = r;
        }
    }

    // If note is no longer active, check if plugin output is silent
    // so we can stop rendering once the release tail is finished
    if (!noteActive) {
        bool isSilent = true;
        const fixed silenceThreshold = i2fp(8); // very small threshold
        for (int i = 0; i < size * 2; i++) {
            fixed s = buffer[i];
            if (s > silenceThreshold || s < -silenceThreshold) {
                isSilent = false;
                break;
            }
        }
        return !isSilent; // return true if there's still audio (release tail)
    }

    return true;
}

void LV2Instrument::ProcessCommand(int channel, FourCC cc, ushort value) {
    if (channel < 0 || channel >= SONG_CHANNEL_COUNT) return;

    // Handle REVB command for reverb effect
    if (cc == I_CMD_REVB) {
        // REVB:aabb - a=decay (0-F), a=damping (0-F), bb=send amount (0-FF)
        unsigned char decayNibble = (value >> 12) & 0x0F;
        unsigned char dampNibble = (value >> 8) & 0x0F;
        unsigned char sendAmount = value & 0xFF;
        
        // Set per-channel reverb parameters
        reverbDecay_[channel] = fl2fp((decayNibble / 15.0f) * 0.9f);
        reverbDamp_[channel] = fl2fp((dampNibble / 15.0f) * 0.85f);
        reverbSend_[channel] = fl2fp(sendAmount / 255.0f);
    } else if (cc == I_CMD_TRML) {
        unsigned char speed = (value >> 8) & 0xFF;
        unsigned char depth = value & 0xFF;
        tremoloLFO_[channel].speed = (speed * 2.0f) / 100.0f; // per sample (100=KRATE)
        tremoloLFO_[channel].depth = depth / 255.0f;
        tremoloLFO_[channel].active = true;
    } else if (cc == I_CMD_VIBR) {
        unsigned char speed = (value >> 8) & 0xFF;
        unsigned char depth = value & 0xFF;
        vibratoLFO_[channel].speed = (speed * 2.0f) / 100.0f;
        vibratoLFO_[channel].depth = depth / 255.0f;
        vibratoLFO_[channel].active = true;
    // ---- MIDI commands routed through QueueMidiEvent ----
    } else if (cc == I_CMD_MVEL) {
        // Set velocity for the next triggered note
        int vel = value / 2;
        if (vel > 127) vel = 127;
        nextVelocity_ = (unsigned char)vel;
    } else if (cc == I_CMD_VOLM) {
        // MIDI CC7 (Volume)
        unsigned char vol = (unsigned char)(value & 0x7F);
        QueueMidiEvent(0xB0, 7, vol);
    } else if (cc == I_CMD_MDCC) {
        // Arbitrary MIDI CC: high byte = CC number, low byte = value
        QueueMidiEvent(0xB0, (value >> 8) & 0x7F, value & 0x7F);
    } else if (cc == I_CMD_MDPG) {
        // MIDI Program Change
        QueueMidiEvent(0xC0, value & 0x7F, 0);
    } else if (cc == I_CMD_LEGA) {
        // Pitch bend — raw 14-bit value from param bytes
        QueueMidiEvent(0xE0, (unsigned char)(value & 0x7F),
                              (unsigned char)((value >> 7) & 0x7F));
    } else if (cc == I_CMD_PFIN) {
        // Pitch bend fine — signed offset centered at 0x2000
        int bend = 0x2000 + (int)(signed short)value;
        if (bend < 0) bend = 0;
        if (bend > 0x3FFF) bend = 0x3FFF;
        QueueMidiEvent(0xE0, (unsigned char)(bend & 0x7F),
                              (unsigned char)((bend >> 7) & 0x7F));
    } else if (cc == I_CMD_PAN_) {
        // MIDI CC10 (Pan)
        QueueMidiEvent(0xB0, 10, (unsigned char)(value & 0x7F));
    } else if (cc == I_CMD_MCAT) {
        // MIDI Channel Aftertouch
        QueueMidiEvent(0xD0, (unsigned char)(value & 0x7F), 0);
    } else if (cc == I_CMD_MPAT) {
        // MIDI Poly Aftertouch: high byte = note, low byte = pressure
        QueueMidiEvent(0xA0, (unsigned char)((value >> 8) & 0x7F),
                              (unsigned char)(value & 0x7F));
    } else if (cc == I_CMD_MBNK) {
        // Bank Select: high byte = MSB (CC0), low byte = LSB (CC32)
        QueueMidiEvent(0xB0, 0, (unsigned char)((value >> 8) & 0x7F));
        QueueMidiEvent(0xB0, 32, (unsigned char)(value & 0x7F));
    } else if (cc == I_CMD_PTCH) {
        // Pitch slide: low byte = signed semitones, assumes ±12 semitone bend range
        int semitones = (int)(signed char)(value & 0xFF);
        int bend = 0x2000 + semitones * (0x2000 / 12);
        if (bend < 0) bend = 0;
        if (bend > 0x3FFF) bend = 0x3FFF;
        QueueMidiEvent(0xE0, (unsigned char)(bend & 0x7F),
                              (unsigned char)((bend >> 7) & 0x7F));
    } else if (cc == I_CMD_FCUT) {
        // Filter cutoff: low byte = 0-FF mapped to CC74 (0-7F)
        QueueMidiEvent(0xB0, 74, (unsigned char)((value & 0xFF) >> 1));
    } else if (cc == I_CMD_FRES) {
        // Filter resonance: low byte = 0-FF mapped to CC71 (0-7F)
        QueueMidiEvent(0xB0, 71, (unsigned char)((value & 0xFF) >> 1));
    } else if (cc == I_CMD_FLTR) {
        // Filter cutoff+resonance: high byte = cutoff, low byte = resonance
        QueueMidiEvent(0xB0, 74, (unsigned char)(((value >> 8) & 0xFF) >> 1));
        QueueMidiEvent(0xB0, 71, (unsigned char)((value & 0xFF) >> 1));
    } else if (cc == I_CMD_ARPG) {
        // Arpeggiate: each table tick advances cycle and retriggers via note-off/on
        LV2ArpState &as = arpState_[channel];
        as.offsets[0] = 0;
        int newLen = 0;
        unsigned int raw = value;
        for (int i = 0; i < 4; i++) {
            as.offsets[4 - i] = raw & 0xF;
            if (as.offsets[4 - i] != 0 && newLen == 0) newLen = 5 - i;
            raw >>= 4;
        }
        as.length = newLen;
        if (!as.active) {
            as.active   = (newLen > 0 && lastNote_[channel] >= 0);
            as.position = 0;
            as.baseNote = lastNote_[channel];
        }
        if (as.active) {
            as.position++;
            if (as.position > as.length) as.position = 0;
            int semitone = (as.position < 5) ? (int)as.offsets[as.position] : 0;
            int newNote  = as.baseNote + semitone;
            if (newNote < 0)   newNote = 0;
            if (newNote > 127) newNote = 127;
            QueueMidiEvent(0x80, (unsigned char)(lastNote_[channel] & 0x7F), 0);
            QueueMidiEvent(0x90, (unsigned char)newNote, 127);
            lastNote_[channel] = newNote;
        }
    } else if (cc == I_CMD_LFOF) {
        unsigned char speed = (value >> 8) & 0xFF;
        unsigned char depth = value & 0xFF;
        filterLFO_[channel].speed  = (speed * 2.0f) / 100.0f;
        filterLFO_[channel].depth  = depth / 255.0f;
        filterLFO_[channel].active = true;
    } else if (cc == I_CMD_CRSH) {
        unsigned char drive = (value >> 8) & 0xFF;
        unsigned char crush = value & 0x0F;
        if (drive > 0) crushDrive_[channel] = drive;
        if (crush > 0) crushBits_[channel]  = crush;
    }
}

void LV2Instrument::QueueMidiEvent(unsigned char status, unsigned char data1, unsigned char data2) {
    unsigned char type = status & 0xF0;

    // User MIDI learn override: intercept CC or PC before passing to plugin
    if (type == 0xB0) {
        int ccNum = data1 & 0x7F;
        auto uit = userCcToParamIdx_.find(ccNum);
        if (uit != userCcToParamIdx_.end()) {
            const LV2PluginParameter *p = GetParameter(uit->second);
            if (p && p->maxValue > p->minValue) {
                float physVal = p->minValue + ((data2 & 0x7F) / 127.0f) * (p->maxValue - p->minValue);
                SetParameterValue(uit->second, physVal);
            }
            return;
        }
    }
    if (type == 0xC0 && programChangeEnabled_) {
        SetPreset(data1 & 0x7F);
        return;
    }

    MidiEvent evt;
    evt.data[0] = status;
    evt.data[1] = data1;
    evt.data[2] = data2;
    // Channel aftertouch and program change are 2-byte messages
    evt.size = (type == 0xC0 || type == 0xD0) ? 2 : 3;

    SysMutexLocker lock(pendingEventsMutex_);
    pendingMidiEvents_.push_back(evt);
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
    // Note: userCcToParamIdx_ and programChangeEnabled_ are NOT cleared on Purge
    // so user MIDI learn settings survive plugin reload.
}

void LV2Instrument::Update(Observable &o, I_ObservableData *d) {
    // Intentionally a no-op.  The audio thread's Render() loop already
    // re-reads every Variable value each cycle, applies scale-point
    // snapping, and writes to portControlStorage_ from the correct thread.
    //
    // Calling SetParameterValue() here from the UI thread caused:
    //   1. A data race on portControlStorage_ (UI write vs audio read)
    //   2. Parameter oscillation for enumerated params (unsnapped value
    //      from UI thread vs snapped value from audio thread alternating
    //      every cycle → audible static/glitches)
    (void)o;
    (void)d;
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
    if (cachedOutputBuffer_) {
        delete[] cachedOutputBuffer_;
        cachedOutputBuffer_ = nullptr;
        cachedOutputSize_ = 0;
    }
    renderChannelMask_ = 0;
    cachedVolVar_ = nullptr;
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
    portsConnected_ = false;
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
                    
                }
                lilv_node_free(name_node);
            }
        }

        // NOTE: We intentionally do NOT send patch:Set atom messages for
        // resource-backed parameters here. The plugin's own instantiation sets
        // up sensible defaults, and flooding the atom buffer with hundreds of
        // patch:Set messages before the first render can prevent the plugin
        // from processing MIDI events.
        

        // Discover presets (file-based for Surge XT/Vital, patchmanager for gearmulator)
        discoverPresets();

        // Force-load preset 0 so the plugin produces sound immediately.
        // Without this, plugins sit in an empty/silent init state until
        // the user manually changes preset.
        if (GetPresetCount() > 0) {
            SetPreset(0);
        }

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
                    
                }
            }
            lilv_nodes_free((LilvNodes*)req);
        }
        if (opt) {
            LILV_FOREACH(nodes, it2, opt) {
                const LilvNode* n = lilv_nodes_get(opt, it2);
                if (n && lilv_node_is_uri(n)) {
                    
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

    // Instantiate the plugin with the actual audio driver sample rate
    double pluginRate = (double)Audio::GetInstance()->GetSampleRate();
    

    LilvInstance* instance = lilv_plugin_instantiate(plugin, pluginRate, g_features);
    if (!instance) {
        Trace::Error("LV2Instrument: Failed to instantiate plugin with full features: %s", pluginURI_);

        // Diagnostic: check plugin library URI and attempt a direct dlopen to capture dlerror
        const LilvNode* lib_node = lilv_plugin_get_library_uri(plugin);
        const char* lib_uri = lib_node ? lilv_node_as_uri(lib_node) : nullptr;
        if (lib_uri) {
            
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
                
                dlclose(h);
            }
        } else {
            
        }

        // Try again with only URID map/unmap to see if extra features are causing the failure
        const LV2_Feature* minimal_features[] = { &g_mapFeature, &g_unmapFeature, nullptr };

        LilvInstance* inst2 = lilv_plugin_instantiate(plugin, pluginRate, minimal_features);
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
    

    // Pre-allocate audio buffers now (not in Render) to avoid heap allocation
    // in the audio callback. Use a fixed size that covers typical block sizes.
    const int BUFFER_SIZE = 2048;
    if (!audioBufferL_) {
        audioBufferL_ = new float[BUFFER_SIZE];
        audioBufferR_ = new float[BUFFER_SIZE];
        audioInputL_ = new float[BUFFER_SIZE];
        audioInputR_ = new float[BUFFER_SIZE];
        audioDummyBuffer_ = new float[BUFFER_SIZE];
        bufferSize_ = BUFFER_SIZE;
        memset(audioBufferL_, 0, BUFFER_SIZE * sizeof(float));
        memset(audioBufferR_, 0, BUFFER_SIZE * sizeof(float));
        memset(audioInputL_, 0, BUFFER_SIZE * sizeof(float));
        memset(audioInputR_, 0, BUFFER_SIZE * sizeof(float));
        memset(audioDummyBuffer_, 0, BUFFER_SIZE * sizeof(float));
        pendingAtomEvents_.reserve(128);
    }
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
                
            }

            // Additionally check if it explicitly lists a buffer type (older plugins); log for diagnostics
            LilvNode* buffer_type_uri = lilv_new_uri(static_cast<LilvWorld*>(world_), "http://lv2plug.in/ns/ext/atom#bufferType");
            const LilvNodes* buf_types = lilv_port_get_value(plugin, port, buffer_type_uri);
            if (buf_types) {
                
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

            // Skip output control ports entirely - they are for the plugin to
            // write to (e.g. level meters) and should not become user-editable
            // Variables.  Creating Variables for them causes the render loop to
            // overwrite the plugin's output values each cycle.
            if (param.isOutput) {
                continue;
            }

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
            
            resourceParamsFound++;
        }
        lilv_nodes_free((LilvNodes*)writable_nodes);
    }
    lilv_node_free(pw_pred);

    // Discovery summary logging
    

    // Synthesize fallback resource URIs for parameters that have no direct control port and no explicit resource URI
    for (size_t pi = 0; pi < parameters_.size(); ++pi) {
        if (parameters_[pi].portIndex < 0 && parameters_[pi].resourceURI.empty()) {
            std::string sanitized = parameters_[pi].name;
            for (auto &ch : sanitized) if (isspace((unsigned char)ch)) ch = '_';
            parameters_[pi].resourceURI = std::string(pluginURI_) + "#" + sanitized;
            
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

    // Ensure atom buffers vector is sized to number of ports.
    // Free any previously allocated buffers first to avoid memory leaks
    // (connectPorts may be called again on buffer grow).
    for (size_t ai = 0; ai < atomInputBuffers_.size(); ++ai) {
        if (atomInputBuffers_[ai]) {
            delete[] atomInputBuffers_[ai];
        }
    }
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
                    
                } else if ((int)i == audioInputPortR_ && audioInputR_) {
                    lilv_instance_connect_port(static_cast<LilvInstance*>(pluginInstance_), i, audioInputR_);
                    
                } else {
                    // Connect to a zeroed input buffer to be safe
                    lilv_instance_connect_port(static_cast<LilvInstance*>(pluginInstance_), i, audioInputL_ ? audioInputL_ : (void*)nullptr);
                    
                }
            } else if (lilv_port_is_a(static_cast<const LilvPlugin*>(plugin_), port, output_class)) {
                // Connect plugin audio output ports to our audio output buffers
                if ((int)i == audioOutputPortL_ && audioBufferL_) {
                    lilv_instance_connect_port(static_cast<LilvInstance*>(pluginInstance_), i, audioBufferL_);
                    
                } else if ((int)i == audioOutputPortR_ && audioBufferR_) {
                    lilv_instance_connect_port(static_cast<LilvInstance*>(pluginInstance_), i, audioBufferR_);
                    
                } else {
                    // Connect extra audio outputs to a separate dummy buffer so they
                    // don't overwrite the real L/R output data.
                    lilv_instance_connect_port(static_cast<LilvInstance*>(pluginInstance_), i, audioDummyBuffer_ ? audioDummyBuffer_ : (void*)nullptr);
                    
                }
            }
            continue;
        }

        // Control ports
        if (lilv_port_is_a(static_cast<const LilvPlugin*>(plugin_), port, control_class)) {
            // Ensure storage exists for this port
            if (portControlStorage_.size() <= i) portControlStorage_.resize(i + 1, 0.0f);
            lilv_instance_connect_port(static_cast<LilvInstance*>(pluginInstance_), i, &portControlStorage_[i]);
            
            continue;
        }

        // Atom ports (MIDI / Atom sequences)
        if (lilv_port_is_a(static_cast<const LilvPlugin*>(plugin_), port, atom_class)) {
            bool isAtomInput = lilv_port_is_a(static_cast<const LilvPlugin*>(plugin_), port, input_class);
            bool isAtomOutput = lilv_port_is_a(static_cast<const LilvPlugin*>(plugin_), port, output_class);
            

            // MIDI input port uses preallocated midiBuffer_
            if ((int)i == midiInputPort_ && midiBuffer_) {
                lilv_instance_connect_port(static_cast<LilvInstance*>(pluginInstance_), i, midiBuffer_);
                
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
            
        }
    }

    

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

            // For atom ports, queue an atom event instead of writing directly
            // to the atom buffer from the UI thread (avoids data race with
            // the audio thread reading the same buffer during plugin run).
            if (parameters_[paramIndex].isAtomPort) {
                // Nothing further here — the atom port value change will be
                // picked up via the patch:Set path below if the parameter has
                // a resource URI, or through portControlStorage_ otherwise.
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
                    {
                        SysMutexLocker lock(pendingEventsMutex_);
                        pendingAtomEvents_.push_back(std::move(ae));
                    }
                    
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

// ====================================================================
// Preset/program accessors
// ====================================================================

const char *LV2Instrument::GetBankName(int bankIdx) const {
    if (bankIdx >= 0 && bankIdx < (int)programLists_.size()) {
        return programLists_[bankIdx].name.c_str();
    }
    return "---";
}

void LV2Instrument::SetCurrentBank(int bankIdx) {
    if (bankIdx >= 0 && bankIdx < (int)programLists_.size()) {
        currentBank_ = bankIdx;
        currentPreset_ = 0;
    }
}

int LV2Instrument::GetPresetCount() const {
    if (currentBank_ >= 0 && currentBank_ < (int)programLists_.size()) {
        return (int)programLists_[currentBank_].programs.size();
    }
    return 0;
}

int LV2Instrument::GetPresetCountForBank(int bankIdx) const {
    if (bankIdx >= 0 && bankIdx < (int)programLists_.size()) {
        return (int)programLists_[bankIdx].programs.size();
    }
    return 0;
}

const char *LV2Instrument::GetPresetName(int presetIdx) const {
    if (currentBank_ >= 0 && currentBank_ < (int)programLists_.size()) {
        const LV2ProgramList &pl = programLists_[currentBank_];
        if (presetIdx >= 0 && presetIdx < (int)pl.programs.size()) {
            return pl.programs[presetIdx].c_str();
        }
    }
    return "---";
}

// Table of known LV2 plugin-to-preset-directory mappings
static const LV2PluginPresetMapping lv2KnownPresetMappings[] = {
    // Surge XT: .fxp files with 60-byte FXP header to skip
    // Uses JUCE string state: <pluginURI>:StateString + JUCE proprietary base64
    {
        "Surge XT",
        {
            "/usr/share/surge-xt/patches_factory",
            "/usr/share/surge-xt/patches_3rdparty",
            nullptr
        },
        ".fxp",
        60,
        false  // useStateBinary = false (JUCE string state)
    },
    // Vital / Vitalium: .vital JSON files, no header to skip
    // Uses JUCE binary state: <urn:juce:stateBinary> + atom:Chunk + standard base64
    {
        "Vital",
        {
            nullptr
        },
        ".vital",
        0,
        true   // useStateBinary = true (JUCE binary state)
    },
    // Sentinel
    { nullptr, {nullptr}, nullptr, 0, false }
};

// ====================================================================
// SetPreset: select a preset
// ====================================================================
void LV2Instrument::SetPreset(int presetIdx) {
    if (currentBank_ < 0 || currentBank_ >= (int)programLists_.size()) return;
    const LV2ProgramList &pl = programLists_[currentBank_];
    if (presetIdx < 0 || presetIdx >= (int)pl.programs.size()) return;

    currentPreset_ = presetIdx;

    // --- File-based preset loading (Surge XT .fxp, Vital .vital) ---
    if (usingFilePresets_) {
        if (currentBank_ < (int)filePresetsByBank_.size() &&
            presetIdx < (int)filePresetsByBank_[currentBank_].size()) {

            const LV2FilePreset &fp = filePresetsByBank_[currentBank_][presetIdx];

            // Determine headerSkipBytes and useStateBinary from known mappings
            int headerSkip = 0;
            bool useStateBinary = false;
            for (int m = 0; lv2KnownPresetMappings[m].pluginNameSubstring != nullptr; m++) {
                if (strstr(name_, lv2KnownPresetMappings[m].pluginNameSubstring) != nullptr) {
                    headerSkip = lv2KnownPresetMappings[m].headerSkipBytes;
                    useStateBinary = lv2KnownPresetMappings[m].useStateBinary;
                    break;
                }
            }

            if (loadPresetFromFile(fp.filePath, headerSkip, useStateBinary)) {
                
            }
        }
        return;
    }

    // --- MIDI-based preset selection (gearmulator/OsTIrus) ---
    // Send bank select LSB (CC#32) + program change as raw MIDI bytes
    // via the atom input port. JUCE converts these to MidiBuffer entries.
    if (usingMidiPresets_) {
        int bankVal = currentBank_;
        if (bankVal < 0) bankVal = 0;
        if (bankVal > 127) bankVal = 127;

        int progVal = presetIdx;
        if (progVal < 0) progVal = 0;
        if (progVal > 127) progVal = 127;

        MidiEvent bankCC;
        bankCC.data[0] = 0xB0;            // Control Change, channel 0
        bankCC.data[1] = 32;              // CC#32 = Bank Select LSB
        bankCC.data[2] = (uint8_t)bankVal;
        bankCC.size = 3;

        MidiEvent progChg;
        progChg.data[0] = 0xC0;            // Program Change, channel 0
        progChg.data[1] = (uint8_t)progVal;
        progChg.data[2] = 0;
        progChg.size = 2;

        {
            SysMutexLocker lock(pendingEventsMutex_);
            pendingMidiEvents_.push_back(bankCC);
            pendingMidiEvents_.push_back(progChg);
        }

        
        return;
    }
}

// ====================================================================
// loadPresetFromFile: load state data from a preset file via lilv_state
// ====================================================================
bool LV2Instrument::loadPresetFromFile(const std::string &filePath, int headerSkipBytes, bool useStateBinary) {
    if (!pluginInstance_ || !world_ || !plugin_) return false;

    // Use POSIX I/O (avoid project fopen macro)
    int fd = open(filePath.c_str(), O_RDONLY);
    if (fd < 0) {
        Trace::Error("LV2: Cannot open preset file: %s", filePath.c_str());
        return false;
    }

    off_t fileSize = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);

    if (fileSize <= headerSkipBytes) {
        Trace::Error("LV2: Preset file too small (%ld bytes): %s", (long)fileSize, filePath.c_str());
        close(fd);
        return false;
    }

    // Skip the header (e.g. 60-byte FXP header for Surge XT)
    if (headerSkipBytes > 0) {
        lseek(fd, headerSkipBytes, SEEK_SET);
    }

    long dataSize = (long)fileSize - headerSkipBytes;
    std::vector<uint8_t> data(dataSize);
    ssize_t bytesRead = read(fd, &data[0], dataSize);
    close(fd);

    if (bytesRead != dataSize) {
        Trace::Error("LV2: Short read on preset file: %s", filePath.c_str());
        return false;
    }

    // Get the plugin URI for the state TTL
    const LilvPlugin* lplug = (const LilvPlugin*)plugin_;
    const LilvNode* plugUri = lilv_plugin_get_uri(lplug);
    const char* plugUriStr = lilv_node_as_uri(plugUri);

    std::string stateTtl;

    if (useStateBinary) {
        // ============================================================
        // JUCE Binary State path (used by Vital)
        // ============================================================
        // Vital's presets.ttl uses:
        //   <urn:juce:stateBinary> [ a atom:Chunk ;
        //       rdf:value "...standard_base64..."^^xsd:base64Binary ; ]
        //
        // The raw file data is encoded with STANDARD RFC 4648 base64.

        static const char stdB64Table[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        // Standard base64 encoding
        std::string b64;
        b64.reserve(((dataSize + 2) / 3) * 4);
        for (long i = 0; i < dataSize; i += 3) {
            uint32_t n = ((uint32_t)data[i]) << 16;
            if (i + 1 < dataSize) n |= ((uint32_t)data[i + 1]) << 8;
            if (i + 2 < dataSize) n |= ((uint32_t)data[i + 2]);

            b64 += stdB64Table[(n >> 18) & 0x3F];
            b64 += stdB64Table[(n >> 12) & 0x3F];
            b64 += (i + 1 < dataSize) ? stdB64Table[(n >> 6) & 0x3F] : '=';
            b64 += (i + 2 < dataSize) ? stdB64Table[n & 0x3F] : '=';
        }

        // Construct TTL matching Vital's presets.ttl structure
        stateTtl += "@prefix atom:  <http://lv2plug.in/ns/ext/atom#> .\n";
        stateTtl += "@prefix lv2:   <http://lv2plug.in/ns/lv2core#> .\n";
        stateTtl += "@prefix pset:  <http://lv2plug.in/ns/ext/presets#> .\n";
        stateTtl += "@prefix rdf:   <http://www.w3.org/1999/02/22-rdf-syntax-ns#> .\n";
        stateTtl += "@prefix rdfs:  <http://www.w3.org/2000/01/rdf-schema#> .\n";
        stateTtl += "@prefix state: <http://lv2plug.in/ns/ext/state#> .\n";
        stateTtl += "@prefix xsd:   <http://www.w3.org/2001/XMLSchema#> .\n\n";
        stateTtl += "[] a pset:Preset ;\n";
        stateTtl += "    lv2:appliesTo <";
        stateTtl += plugUriStr;
        stateTtl += "> ;\n";
        stateTtl += "    state:state [\n";
        stateTtl += "        <urn:juce:stateBinary> [\n";
        stateTtl += "            a atom:Chunk ;\n";
        stateTtl += "            rdf:value \"";
        stateTtl += b64;
        stateTtl += "\"^^xsd:base64Binary ;\n";
        stateTtl += "        ] ;\n";
        stateTtl += "    ] .\n";
    } else {
        // ============================================================
        // JUCE String State path (used by Surge XT)
        // ============================================================
        // JUCE's LV2 wrapper stores state under the URI:
        //   <JucePlugin_LV2URI>:StateString   (colon separator, "StateString" suffix)
        // with type atom:String (LV2_ATOM__String).
        //
        // The data is encoded using JUCE's proprietary MemoryBlock::toBase64Encoding()
        // format:  "<decimal_byte_count>.<juce_base64_chars>"
        // The charset is: .ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+
        // (note: starts with '.' at index 0, NOT standard RFC 4648 base64)

        static const char juceB64Table[] =
            ".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+";

        // Helper: read numBits bits from the data bit-stream starting at bitStart.
        // This replicates JUCE's MemoryBlock::getBitRange() which uses LSB-first
        // bit ordering within each byte and packs bits LSB-first into the result.
        auto getBitRange = [&](const uint8_t* buf, long bufSize, long bitStart, int numBits) -> int {
            int res = 0;
            long byte = bitStart >> 3;
            long offsetInByte = bitStart & 7;
            int bitsSoFar = 0;

            while (numBits > 0 && byte < bufSize) {
                int bitsThisTime = numBits < (int)(8 - offsetInByte) ? numBits : (int)(8 - offsetInByte);
                int mask = (0xff >> (8 - bitsThisTime)) << offsetInByte;
                res |= (((buf[byte] & mask) >> offsetInByte) << bitsSoFar);
                bitsSoFar += bitsThisTime;
                numBits -= bitsThisTime;
                ++byte;
                offsetInByte = 0;
            }
            return res;
        };

        // Encode using JUCE's format: "<size>.<encoded_chars>"
        long numChars = ((dataSize * 8) + 5) / 6;
        std::string juceB64;
        juceB64 = std::to_string(dataSize);
        juceB64 += '.';
        juceB64.reserve(juceB64.size() + numChars + 1);
        for (long i = 0; i < numChars; i++) {
            int sixBits = getBitRange(&data[0], dataSize, i * 6, 6);
            juceB64 += juceB64Table[sixBits & 0x3F];
        }

        // Construct a minimal Turtle state string.
        // JUCE LV2 state URI = <pluginURI>:StateString
        // JUCE LV2 state type = atom:String (LV2_ATOM__String)
        stateTtl += "@prefix atom: <http://lv2plug.in/ns/ext/atom#> .\n";
        stateTtl += "@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n";
        stateTtl += "@prefix pset: <http://lv2plug.in/ns/ext/presets#> .\n";
        stateTtl += "@prefix state: <http://lv2plug.in/ns/ext/state#> .\n\n";
        stateTtl += "[] a pset:Preset ;\n";
        stateTtl += "    lv2:appliesTo <";
        stateTtl += plugUriStr;
        stateTtl += "> ;\n";
        stateTtl += "    state:state [\n";
        stateTtl += "        <";
        stateTtl += plugUriStr;
        stateTtl += ":StateString> \"";
        stateTtl += juceB64;
        // NOTE: Do NOT add ^^atom:String here!  sratom treats typed literals
        // (with ^^<type>) differently from plain literals.  A typed literal with
        // atom:String would be read by sratom as forge.Literal (since atom:String
        // doesn't match any of sratom's known XSD types), but JUCE expects
        // forge.String.  A plain untyped literal causes sratom to call
        // lv2_atom_forge_string(), producing the correct atom type (forge.String =
        // URID for LV2_ATOM__String), which is what JUCE checks for in its
        // retrieve() callback.
        stateTtl += "\"\n";
        stateTtl += "    ] .\n";
    }

    LilvWorld* world = (LilvWorld*)world_;
    LilvState* state = lilv_state_new_from_string(world, &g_uridMapFeature, stateTtl.c_str());
    if (!state) {
        Trace::Error("LV2: Failed to parse state TTL for preset: %s", filePath.c_str());
        return false;
    }

    LilvInstance* instance = (LilvInstance*)pluginInstance_;
    lilv_state_restore(state, instance, NULL, NULL, 0, NULL);
    lilv_state_free(state);

    
    return true;
}

// ====================================================================
// savePresetToFile: save current plugin state to a native preset file
// ====================================================================
bool LV2Instrument::savePresetToFile(const std::string &filePath) {
    if (!pluginInstance_ || !world_ || !plugin_) {
        Trace::Error("LV2: Cannot save preset – plugin not loaded");
        return false;
    }

    // Find the mapping for this plugin
    const LV2PluginPresetMapping *mapping = nullptr;
    for (int i = 0; lv2KnownPresetMappings[i].pluginNameSubstring != nullptr; i++) {
        if (strstr(name_, lv2KnownPresetMappings[i].pluginNameSubstring) != nullptr) {
            mapping = &lv2KnownPresetMappings[i];
            break;
        }
    }
    if (!mapping) {
        Trace::Error("LV2: No preset mapping for '%s' – cannot save", name_);
        return false;
    }

    // Capture current plugin state using lilv
    LilvWorld *world = (LilvWorld *)world_;
    LilvInstance *instance = (LilvInstance *)pluginInstance_;
    const LilvPlugin *lplug = (const LilvPlugin *)plugin_;

    LilvState *state = lilv_state_new_from_instance(
        lplug, instance, &g_uridMapFeature,
        nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, LV2_STATE_IS_POD, nullptr);

    if (!state) {
        Trace::Error("LV2: lilv_state_new_from_instance failed");
        return false;
    }

    // Serialize state to TTL string
    char *ttlStr = lilv_state_to_string(
        world, &g_uridMapFeature, &g_uridUnmapFeature,
        state, "http://temp/state", nullptr);

    lilv_state_free(state);

    if (!ttlStr) {
        Trace::Error("LV2: lilv_state_to_string failed");
        return false;
    }

    std::string ttl(ttlStr);
    free(ttlStr);

    

    // Extract the base64-encoded data from the TTL
    std::vector<uint8_t> rawData;

    if (mapping->useStateBinary) {
        // ============================================================
        // Binary state path (Vital): extract standard base64 from
        //   rdf:value "...base64..."^^xsd:base64Binary
        // ============================================================
        std::string marker = "rdf:value \"";
        size_t pos = ttl.find(marker);
        if (pos == std::string::npos) {
            // Try alternative: just find base64Binary typed literal
            marker = "\"";
            pos = ttl.find("^^xsd:base64Binary");
            if (pos != std::string::npos) {
                // Search backwards from ^^ to find the opening quote
                size_t qqEnd = pos;
                size_t qqStart = ttl.rfind('"', qqEnd - 1);
                if (qqStart != std::string::npos) {
                    std::string b64 = ttl.substr(qqStart + 1, qqEnd - qqStart - 1);
                    // Standard base64 decode
                    static const int stdB64Decode[256] = {
                        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
                        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
                        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
                        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
                        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
                        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
                        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
                    };
                    for (size_t i = 0; i < b64.size(); i += 4) {
                        int a = (i < b64.size()) ? stdB64Decode[(unsigned char)b64[i]] : -1;
                        int b = (i+1 < b64.size()) ? stdB64Decode[(unsigned char)b64[i+1]] : -1;
                        int c = (i+2 < b64.size()) ? stdB64Decode[(unsigned char)b64[i+2]] : -1;
                        int d = (i+3 < b64.size()) ? stdB64Decode[(unsigned char)b64[i+3]] : -1;
                        if (a < 0 || b < 0) break;
                        rawData.push_back((a << 2) | (b >> 4));
                        if (c >= 0) rawData.push_back(((b & 0xF) << 4) | (c >> 2));
                        if (d >= 0) rawData.push_back(((c & 0x3) << 6) | d);
                    }
                }
            }
        } else {
            pos += marker.size();
            size_t endPos = ttl.find('"', pos);
            if (endPos != std::string::npos) {
                std::string b64 = ttl.substr(pos, endPos - pos);
                // Standard base64 decode
                static const int stdB64Decode[256] = {
                    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
                    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
                    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
                    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
                    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
                    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
                    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
                };
                for (size_t i = 0; i < b64.size(); i += 4) {
                    int a = (i < b64.size()) ? stdB64Decode[(unsigned char)b64[i]] : -1;
                    int b = (i+1 < b64.size()) ? stdB64Decode[(unsigned char)b64[i+1]] : -1;
                    int c = (i+2 < b64.size()) ? stdB64Decode[(unsigned char)b64[i+2]] : -1;
                    int d = (i+3 < b64.size()) ? stdB64Decode[(unsigned char)b64[i+3]] : -1;
                    if (a < 0 || b < 0) break;
                    rawData.push_back((a << 2) | (b >> 4));
                    if (c >= 0) rawData.push_back(((b & 0xF) << 4) | (c >> 2));
                    if (d >= 0) rawData.push_back(((c & 0x3) << 6) | d);
                }
            }
        }
    } else {
        // ============================================================
        // JUCE String State path (Surge XT): extract JUCE proprietary base64 from
        //   <pluginURI>:StateString "size.encoded_data"
        // ============================================================
        std::string stateKey = ":StateString";
        size_t pos = ttl.find(stateKey);
        if (pos == std::string::npos) {
            Trace::Error("LV2: StateString key not found in state TTL");
            return false;
        }
        // Find the quoted value after the key
        pos = ttl.find('"', pos + stateKey.size());
        if (pos == std::string::npos) {
            Trace::Error("LV2: No quoted value after StateString key");
            return false;
        }
        pos++; // skip opening quote
        size_t endPos = ttl.find('"', pos);
        if (endPos == std::string::npos) {
            Trace::Error("LV2: No closing quote for StateString value");
            return false;
        }
        std::string juceB64 = ttl.substr(pos, endPos - pos);

        // Parse JUCE format: "<decimal_byte_count>.<encoded_chars>"
        size_t dotPos = juceB64.find('.');
        if (dotPos == std::string::npos) {
            Trace::Error("LV2: Invalid JUCE base64 format (no dot)");
            return false;
        }
        long byteCount = atol(juceB64.substr(0, dotPos).c_str());
        std::string encoded = juceB64.substr(dotPos + 1);

        // JUCE proprietary base64 charset (LSB-first, index 0 = '.')
        static const char juceB64Table[] =
            ".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+";
        int juceB64Decode[256];
        memset(juceB64Decode, -1, sizeof(juceB64Decode));
        for (int i = 0; i < 64; i++) {
            juceB64Decode[(unsigned char)juceB64Table[i]] = i;
        }

        // Decode JUCE proprietary base64: LSB-first 6-bit packing
        rawData.resize(byteCount, 0);
        for (size_t i = 0; i < encoded.size(); i++) {
            int sixBits = juceB64Decode[(unsigned char)encoded[i]];
            if (sixBits < 0) continue;
            // Write 6 bits starting at bit position i*6, LSB-first
            long bitStart = (long)i * 6;
            for (int b = 0; b < 6; b++) {
                if (sixBits & (1 << b)) {
                    long bitPos = bitStart + b;
                    long byteIdx = bitPos >> 3;
                    int bitInByte = bitPos & 7;
                    if (byteIdx < byteCount) {
                        rawData[byteIdx] |= (1 << bitInByte);
                    }
                }
            }
        }
    }

    if (rawData.empty()) {
        Trace::Error("LV2: No state data decoded from plugin state");
        return false;
    }

    

    // Write the file
    int fd = open(filePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        Trace::Error("LV2: Cannot create preset file: %s", filePath.c_str());
        return false;
    }

    // For Surge XT, prepend a 60-byte FXP header
    if (mapping->headerSkipBytes > 0) {
        // FXP header for Surge XT (60 bytes)
        uint8_t fxpHeader[60];
        memset(fxpHeader, 0, 60);

        // FXP magic: "CcnK" (big-endian)
        fxpHeader[0] = 'C'; fxpHeader[1] = 'c'; fxpHeader[2] = 'n'; fxpHeader[3] = 'K';

        // byteSize: total file size - 8 (big-endian 32-bit)
        uint32_t byteSize = (uint32_t)(60 - 8 + rawData.size());
        fxpHeader[4] = (byteSize >> 24) & 0xFF;
        fxpHeader[5] = (byteSize >> 16) & 0xFF;
        fxpHeader[6] = (byteSize >> 8) & 0xFF;
        fxpHeader[7] = byteSize & 0xFF;

        // fxMagic: "FPCh" for opaque chunk (big-endian)
        fxpHeader[8] = 'F'; fxpHeader[9] = 'P'; fxpHeader[10] = 'C'; fxpHeader[11] = 'h';

        // version: 1 (big-endian)
        fxpHeader[15] = 1;

        // fxID: "csST" for Surge XT (big-endian)
        fxpHeader[16] = 'c'; fxpHeader[17] = 's'; fxpHeader[18] = 'S'; fxpHeader[19] = 'T';

        // fxVersion: 1 (big-endian)
        fxpHeader[23] = 1;

        // numParams: 0 (opaque chunk mode)
        // bytes 24-27 = 0

        // prgName: up to 28 chars at offset 28
        // Extract name from file path for the FXP program name
        std::string fname = filePath;
        size_t slashPos = fname.rfind('/');
        if (slashPos != std::string::npos) fname = fname.substr(slashPos + 1);
        size_t dotPos = fname.rfind('.');
        if (dotPos != std::string::npos) fname = fname.substr(0, dotPos);
        strncpy((char *)&fxpHeader[28], fname.c_str(), 27);
        fxpHeader[55] = 0;

        // chunkSize: size of the chunk data (big-endian 32-bit) at offset 56
        uint32_t chunkSize = (uint32_t)rawData.size();
        fxpHeader[56] = (chunkSize >> 24) & 0xFF;
        fxpHeader[57] = (chunkSize >> 16) & 0xFF;
        fxpHeader[58] = (chunkSize >> 8) & 0xFF;
        fxpHeader[59] = chunkSize & 0xFF;

        ssize_t w = write(fd, fxpHeader, 60);
        (void)w;
    }

    // Write the raw state data
    ssize_t w = write(fd, rawData.data(), rawData.size());
    (void)w;
    close(fd);

    
    return true;
}

// ====================================================================
// getCurrentBankDirectory: get directory path for the current bank
// ====================================================================
std::string LV2Instrument::getCurrentBankDirectory() const {
    if (!usingFilePresets_) return "";
    if (currentBank_ < 0 || currentBank_ >= (int)filePresetsByBank_.size()) return "";
    if (filePresetsByBank_[currentBank_].empty()) return "";

    // Get directory from the first preset in the current bank
    const std::string &path = filePresetsByBank_[currentBank_][0].filePath;
    size_t slashPos = path.rfind('/');
    if (slashPos != std::string::npos) {
        return path.substr(0, slashPos);
    }
    return "";
}

// ====================================================================
// getPresetExtension: get native file extension for this plugin
// ====================================================================
const char *LV2Instrument::getPresetExtension() const {
    for (int i = 0; lv2KnownPresetMappings[i].pluginNameSubstring != nullptr; i++) {
        if (strstr(name_, lv2KnownPresetMappings[i].pluginNameSubstring) != nullptr) {
            return lv2KnownPresetMappings[i].extension;
        }
    }
    return nullptr;
}

// ====================================================================
// canSavePreset: check if preset saving is supported for this plugin
// ====================================================================
bool LV2Instrument::canSavePreset() const {
    if (!usingFilePresets_ || !pluginInstance_) return false;
    return getPresetExtension() != nullptr;
}

// ====================================================================
// refreshPresets: re-scan preset files and update lists
// ====================================================================
void LV2Instrument::refreshPresets() {
    int savedBank = currentBank_;
    discoverPresetFiles();
    // Try to restore the bank position
    if (savedBank >= 0 && savedBank < (int)programLists_.size()) {
        currentBank_ = savedBank;
    }
    // Set preset to the last one (the newly saved one is likely at the end)
    int count = GetPresetCountForBank(currentBank_);
    if (count > 0) {
        currentPreset_ = count - 1;
    }
}

// ====================================================================
// discoverPresets: main preset discovery entry point
// ====================================================================
void LV2Instrument::discoverPresets() {
    programLists_.clear();
    filePresetsByBank_.clear();
    usingFilePresets_ = false;
    usingMidiPresets_ = false;
    currentBank_ = 0;
    currentPreset_ = 0;

    // Try file-based preset scanning (Surge XT .fxp, Vital .vital)
    discoverPresetFiles();

    // If no file presets found, try hardcoded ROM presets (OsTIrus)
    if (!usingFilePresets_) {
        discoverHardcodedPresets();
    }

    // If no hardcoded presets either, try patchmanager cache (other gearmulator)
    if (!usingFilePresets_ && !usingMidiPresets_) {
        discoverPatchManagerPresets();
    }
}

// ====================================================================
// discoverPresetFiles: scan filesystem for native preset files
// ====================================================================

// Helper: recursively find files with a given extension in a directory
static void lv2FindPresetFiles(const std::string &dir, const char *extension,
                               const std::string &category,
                               std::map<std::string, std::vector<LV2FilePreset>> &bankMap) {
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    struct dirent *ent;
    size_t extLen = strlen(extension);

    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        std::string name(ent->d_name);
        std::string full = dir + "/" + name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            std::string subCat = category.empty() ? name : category + "/" + name;
            lv2FindPresetFiles(full, extension, subCat, bankMap);
        } else if (S_ISREG(st.st_mode)) {
            if (name.size() > extLen &&
                name.substr(name.size() - extLen) == extension) {
                LV2FilePreset fp;
                fp.name = name.substr(0, name.size() - extLen);
                fp.filePath = full;
                std::string bankName = category.empty() ? "Presets" : category;
                bankMap[bankName].push_back(fp);
            }
        }
    }
    closedir(d);
}

void LV2Instrument::discoverPresetFiles() {
    const LV2PluginPresetMapping *mapping = nullptr;
    for (int i = 0; lv2KnownPresetMappings[i].pluginNameSubstring != nullptr; i++) {
        if (strstr(name_, lv2KnownPresetMappings[i].pluginNameSubstring) != nullptr) {
            mapping = &lv2KnownPresetMappings[i];
            break;
        }
    }

    if (!mapping) {
        
        return;
    }

    

    std::string homeDir;
    const char *home = getenv("HOME");
    if (home) homeDir = home;

    std::vector<std::string> scanDirs;
    for (int i = 0; i < 6 && mapping->directories[i] != nullptr; i++) {
        scanDirs.push_back(mapping->directories[i]);
    }

    // Add user-specific directories
    if (!homeDir.empty()) {
        if (strstr(name_, "Surge XT") != nullptr) {
            scanDirs.push_back(homeDir + "/Documents/Surge XT/Patches");
        } else if (strstr(name_, "Vital") != nullptr) {
            scanDirs.push_back(homeDir + "/Documents/Vital");
            scanDirs.push_back(homeDir + "/.vital/User/Presets");
            scanDirs.push_back(homeDir + "/Music/Vital");
        }
    }

    std::map<std::string, std::vector<LV2FilePreset>> bankMap;
    for (size_t i = 0; i < scanDirs.size(); i++) {
        lv2FindPresetFiles(scanDirs[i], mapping->extension, "", bankMap);
    }

    if (bankMap.empty()) {
        
        return;
    }

    programLists_.clear();
    filePresetsByBank_.clear();
    usingFilePresets_ = true;

    for (std::map<std::string, std::vector<LV2FilePreset>>::iterator it = bankMap.begin();
         it != bankMap.end(); ++it) {
        LV2ProgramList pl;
        pl.listId = (int32_t)programLists_.size();
        pl.name = it->first;

        std::vector<LV2FilePreset> bankFilePresets;
        for (size_t j = 0; j < it->second.size(); j++) {
            pl.programs.push_back(it->second[j].name);
            bankFilePresets.push_back(it->second[j]);
        }

        programLists_.push_back(pl);
        filePresetsByBank_.push_back(bankFilePresets);
    }

    currentBank_ = 0;
    currentPreset_ = 0;

    
    for (size_t i = 0; i < programLists_.size(); i++) {
        
    }
}

// ====================================================================
// discoverHardcodedPresets: use built-in ROM patch names for OsTIrus
// ====================================================================
void LV2Instrument::discoverHardcodedPresets() {
    if (strstr(name_, "OsTIrus") == nullptr) return;

    

    programLists_.clear();
    for (int b = 0; b < OSTIRUS_BANK_COUNT; b++) {
        LV2ProgramList pl;
        pl.listId = b;
        pl.name = ostirusBankNames[b];
        for (int p = 0; p < OSTIRUS_PATCHES_PER_BANK; p++) {
            pl.programs.push_back(ostirusPatchNames[b][p]);
        }
        programLists_.push_back(pl);
    }

    usingMidiPresets_ = true;
    currentBank_ = 0;
    currentPreset_ = 0;

    
}

// ====================================================================
// discoverPatchManagerPresets: parse gearmulator patchmanager cache
// for OsTIrus/Osirus/etc. LV2 plugins
// ====================================================================

struct LV2PatchManagerMapping {
    const char *pluginNameSubstring;
    const char *cacheDirSuffix;
};

static const LV2PatchManagerMapping lv2PatchManagerMappings[] = {
    { "OsTIrus",  "OsTIrus" },
    { "Osirus",   "Osirus" },
    { "Vavra",    "Vavra" },
    { "Xenia",    "Xenia" },
    { nullptr, nullptr }
};

void LV2Instrument::discoverPatchManagerPresets() {
    const LV2PatchManagerMapping *mapping = nullptr;
    for (int i = 0; lv2PatchManagerMappings[i].pluginNameSubstring != nullptr; i++) {
        if (strstr(name_, lv2PatchManagerMappings[i].pluginNameSubstring) != nullptr) {
            mapping = &lv2PatchManagerMappings[i];
            break;
        }
    }
    if (!mapping) return;

    const char *home = getenv("HOME");
    if (!home) return;

    std::string cachePath = std::string(home) +
        "/.local/share/The Usual Suspects/" +
        mapping->cacheDirSuffix +
        "/patchmanager/patchmanagerdb.cache";

    

    int fd = open(cachePath.c_str(), O_RDONLY);
    if (fd < 0) {
        
        return;
    }

    off_t fileSize = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);

    if (fileSize < 28) {
        Trace::Error("LV2: Patchmanager cache too small (%ld bytes)", (long)fileSize);
        close(fd);
        return;
    }

    std::vector<uint8_t> data(fileSize);
    ssize_t bytesRead = read(fd, &data[0], fileSize);
    close(fd);

    if (bytesRead != fileSize) {
        Trace::Error("LV2: Short read on patchmanager cache");
        return;
    }

    const uint8_t *buf = &data[0];
    size_t total = (size_t)fileSize;
    size_t pos = 0;

    auto readU32 = [&](uint32_t &val) -> bool {
        if (pos + 4 > total) return false;
        val = (uint32_t)buf[pos] | ((uint32_t)buf[pos+1] << 8) |
              ((uint32_t)buf[pos+2] << 16) | ((uint32_t)buf[pos+3] << 24);
        pos += 4;
        return true;
    };
    auto readU16 = [&](uint16_t &val) -> bool {
        if (pos + 2 > total) return false;
        val = (uint16_t)buf[pos] | ((uint16_t)buf[pos+1] << 8);
        pos += 2;
        return true;
    };
    auto readTag = [&](char tag[5]) -> bool {
        if (pos + 4 > total) return false;
        tag[0] = buf[pos]; tag[1] = buf[pos+1];
        tag[2] = buf[pos+2]; tag[3] = buf[pos+3]; tag[4] = '\0';
        pos += 4;
        return true;
    };

    // Parse file header: Pmpm + version + totalSize
    char tag[5];
    uint32_t ver, sz;
    if (!readTag(tag) || strcmp(tag, "Pmpm") != 0) {
        Trace::Error("LV2: Invalid patchmanager cache header");
        return;
    }
    if (!readU32(ver) || !readU32(sz)) return;

    // Parse PmDs container
    if (!readTag(tag) || strcmp(tag, "PmDs") != 0) {
        Trace::Error("LV2: Expected PmDs container in patchmanager cache");
        return;
    }
    uint32_t pmdsVer, pmdsSize, bankCount;
    if (!readU32(pmdsVer) || !readU32(pmdsSize) || !readU32(bankCount)) return;

    

    if (bankCount > 256) {
        Trace::Error("LV2: Unreasonable bank count %d", (int)bankCount);
        return;
    }

    programLists_.clear();
    filePresetsByBank_.clear();

    for (uint32_t b = 0; b < bankCount; b++) {
        if (!readTag(tag) || strcmp(tag, "DatS") != 0) {
            Trace::Error("LV2: Expected DatS at bank %d, got '%s'", (int)b, tag);
            return;
        }
        uint32_t dsVer, dsSize;
        if (!readU32(dsVer) || !readU32(dsSize)) return;
        size_t payloadStart = pos;

        uint16_t flag, nameLen, pad16;
        if (!readU16(flag) || !readU16(nameLen) || !readU16(pad16)) {
            pos = payloadStart + dsSize;
            continue;
        }

        char bankNameBuf[64];
        if (nameLen > 63) nameLen = 63;
        if (pos + nameLen > total) {
            pos = payloadStart + dsSize;
            continue;
        }
        memcpy(bankNameBuf, buf + pos, nameLen);
        bankNameBuf[nameLen] = '\0';
        for (int i = (int)nameLen - 1; i >= 0 && (bankNameBuf[i] == '\0' || bankNameBuf[i] == ' '); i--)
            bankNameBuf[i] = '\0';
        pos += nameLen;

        uint32_t pad32, patchCount;
        if (!readU32(pad32) || !readU32(patchCount)) {
            pos = payloadStart + dsSize;
            continue;
        }

        LV2ProgramList pl;
        pl.listId = (int32_t)b;
        pl.name = bankNameBuf;

        for (uint32_t p = 0; p < patchCount && pos < total; p++) {
            if (!readTag(tag) || strcmp(tag, "Patc") != 0) {
                Trace::Error("LV2: Expected Patc at bank %d patch %d, got '%s'", (int)b, (int)p, tag);
                pos = payloadStart + dsSize;
                break;
            }
            uint32_t pVer, pSize;
            if (!readU32(pVer) || !readU32(pSize)) {
                pos = payloadStart + dsSize;
                break;
            }
            size_t pPayload = pos;

            uint32_t pNameLen;
            if (!readU32(pNameLen)) {
                pos = pPayload + pSize;
                continue;
            }
            char patchNameBuf[32];
            uint32_t copyLen = pNameLen;
            if (copyLen > 31) copyLen = 31;
            if (pos + copyLen > total) {
                pos = pPayload + pSize;
                continue;
            }
            memcpy(patchNameBuf, buf + pos, copyLen);
            patchNameBuf[copyLen] = '\0';
            for (int i = (int)copyLen - 1; i >= 0 && (patchNameBuf[i] == '\0' || patchNameBuf[i] == ' '); i--)
                patchNameBuf[i] = '\0';

            pl.programs.push_back(patchNameBuf);
            pos = pPayload + pSize;
        }

        if (pl.programs.size() > 0) {
            programLists_.push_back(pl);
        }

        pos = payloadStart + dsSize;
    }

    if (programLists_.empty()) {
        
        return;
    }

    // Preset selection via MIDI bank select + program change
    usingMidiPresets_ = true;
    currentBank_ = 0;
    currentPreset_ = 0;

    
    for (size_t i = 0; i < programLists_.size() && i < 5; i++) {
        
    }
    if (programLists_.size() > 5) {
        
    }
}

// ====================================================================
// User MIDI learn: CC → parameter index bindings
// ====================================================================

void LV2Instrument::SetUserCC(int cc, int paramIdx) {
    for (auto it = userCcToParamIdx_.begin(); it != userCcToParamIdx_.end(); ) {
        if (it->second == paramIdx) it = userCcToParamIdx_.erase(it);
        else ++it;
    }
    userCcToParamIdx_[cc] = paramIdx;
}

void LV2Instrument::ClearUserCCForParam(int paramIdx) {
    for (auto it = userCcToParamIdx_.begin(); it != userCcToParamIdx_.end(); ) {
        if (it->second == paramIdx) it = userCcToParamIdx_.erase(it);
        else ++it;
    }
}

int LV2Instrument::GetUserCCForParam(int paramIdx) const {
    for (const auto &kv : userCcToParamIdx_) {
        if (kv.second == paramIdx) return kv.first;
    }
    return -1;
}

void LV2Instrument::ApplyUserCC(int ccNum, int rawValue) {
    auto it = userCcToParamIdx_.find(ccNum);
    if (it != userCcToParamIdx_.end()) {
        int paramIdx = it->second;
        const LV2PluginParameter *p = GetParameter(paramIdx);
        if (p && p->maxValue > p->minValue) {
            float physVal = p->minValue + ((rawValue & 0x7F) / 127.0f) * (p->maxValue - p->minValue);
            SetParameterValue(paramIdx, physVal);
            // Also update the Variable so the audio thread's variable scan doesn't revert our change
            if (paramIdx < (int)parameters_.size() && parameters_[paramIdx].variable) {
                int scaledVal = (int)(((physVal - p->minValue) / (p->maxValue - p->minValue)) * 127.0f + 0.5f);
                if (scaledVal < 0) scaledVal = 0;
                if (scaledVal > 127) scaledVal = 127;
                parameters_[paramIdx].variable->SetInt(scaledVal, false);
            }
        }
    }
}
#include "SoundFontInstrument.h"
#include "SoundFontManager.h"
#include "SamplePool.h"
#include "CommandList.h"
#include "System/Console/Trace.h"
#include "System/io/Status.h"
#include "Services/Audio/Audio.h"
#include "Application/Player/SyncMaster.h"
#include "Externals/Soundfont/ENAB.H"

#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>

#define KRATE_SAMPLE_COUNT 100
// De-click fade length in samples (~1.5ms at 44.1kHz)
#define SF_DECLICK_FADE 64

// ─────────────────────────────────────────────────────────────────────────────
//  SF2 unit conversions
// ─────────────────────────────────────────────────────────────────────────────

// SF2 timecents → seconds: t = 2^(tc/1200)
// Then multiply by sampleRate to get samples.
// tc = -32768 is treated as "instantaneous" (0 samples).
double SoundFontInstrument::timecentsToSamples(short tc, double sampleRate) {
    if (tc <= -32768) return 0.0;
    double seconds = pow(2.0, tc / 1200.0);
    return seconds * sampleRate;
}

// SF2 centibels → linear gain: gain = 10^(-cb/200)
// cb=0 → gain=1.0, cb=60 → gain≈0.5, cb=1440 → effectively silent.
float SoundFontInstrument::centibelToGain(short cb) {
    if (cb <= 0) return 1.0f;
    if (cb >= 1440) return 0.0f;
    return (float)pow(10.0, -cb / 200.0);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

SoundFontInstrument::SoundFontInstrument() {
    sf2Path_[0] = '\0';
    strcpy(sf2Name_, "SF2");
    bankID_ = -1;
    presetCount_ = 0;
    currentPreset_ = -1;
    presetSource_ = nullptr;

    // Initialize per-channel state
    for (int c = 0; c < SONG_CHANNEL_COUNT; c++) {
        channels_[c].finished = true;
        channels_[c].midiNote = 60;
        channels_[c].voiceCount = 0;
        channels_[c].fadeOutSamples = 0;
        channels_[c].fadeOutTotal = 0;
        for (int v = 0; v < SF_MAX_LAYERS; v++) {
            channels_[c].voices[v].active = false;
        }
    }

    // Create UI variables
    volume_ = new Variable("volume", SFIP_VOLUME, 0xFF);
    Insert(volume_);

    pan_ = new Variable("pan", SFIP_PAN, 0x7F);
    Insert(pan_);

    table_ = new Variable("table", SFIP_TABLE, -1);
    Insert(table_);

    tableAuto_ = new Variable("table automation", SFIP_TABLEAUTO, false);
    Insert(tableAuto_);

    presetVar_ = new WatchedVariable("preset", SFIP_PRESET, 0);
    Insert(presetVar_);
}

SoundFontInstrument::~SoundFontInstrument() {
    // Unload the SF2 bank
    if (bankID_ >= 0) {
        SoundFontManager::GetInstance()->UnloadBank(bankID_);
        bankID_ = -1;
    }
    delete presetSource_;
    presetSource_ = nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
//  I_Instrument interface
// ─────────────────────────────────────────────────────────────────────────────

bool SoundFontInstrument::Init() {
    // Apply any pending variables from project restore
    if (!pendingParamValues_.empty()) {
        // Restore the SF2 file path if stored
        auto itFile = pendingParamValues_.find("sf2file");
        if (itFile != pendingParamValues_.end() && !itFile->second.empty()) {
            SetSF2File(itFile->second.c_str());
        }
        // Restore preset index
        auto itPreset = pendingParamValues_.find("preset");
        if (itPreset != pendingParamValues_.end()) {
            int pi = atoi(itPreset->second.c_str());
            if (pi >= 0 && pi < presetCount_) {
                SelectPreset(pi);
            }
        }
        // Restore standard variables
        for (auto &pv : pendingParamValues_) {
            if (pv.first == "sf2file" || pv.first == "preset") continue;
            IteratorPtr<Variable> vi(GetIterator());
            for (vi->Begin(); !vi->IsDone(); vi->Next()) {
                Variable &v = vi->CurrentItem();
                if (!strcmp(v.GetName(), pv.first.c_str())) {
                    v.SetString(pv.second.c_str());
                    break;
                }
            }
        }
        pendingParamValues_.clear();
    }
    return true;
}

bool SoundFontInstrument::Start(int channel, unsigned char note, bool retrigger) {
    if (!presetSource_ || channel < 0 || channel >= SONG_CHANNEL_COUNT) return false;

    SFChannelState &cs = channels_[channel];
    cs.midiNote = note;
    cs.finished = false;
    cs.fadeOutSamples = 0;
    cs.fadeOutTotal = 0;

    // Reset LFO modulation on new note
    cs.tremolo.active = false;
    cs.tremolo.phase = 0;
    cs.vibrato.active = false;
    cs.vibrato.phase = 0;
    cs.lfoFilter.active = false;
    cs.lfoFilter.phase = 0;

    float driverRate = (float)Audio::GetInstance()->GetSampleRate();

    // Navigate the SF2 preset for this note + velocity (use velocity 127 for
    // now; the user volume is applied separately)
    int velocity = 127;

    // Navigate the SF2 preset using the hydra navigator to get all
    // oscillators (layers/splits) for this note+velocity combination.
    HydraClass *hydra = GetHydraPtr(bankID_);
    if (!hydra) {
        cs.finished = true;
        return false;
    }

    SoundFontNavigator tempNav;
    tempNav.SetHydraFont(hydra);
    tempNav.Navigate(currentPreset_, note, velocity);
    int nOsc = tempNav.GetNOsc();
    sfData *sfPtr = tempNav.GetSFPtr();

    if (nOsc <= 0 || !sfPtr) {
        cs.finished = true;
        return false;
    }

    // Clamp to max voice count
    if (nOsc > SF_MAX_LAYERS) nOsc = SF_MAX_LAYERS;

    cs.voiceCount = 0;

    for (int osc = 0; osc < nOsc; osc++) {
        sfData &sf = sfPtr[osc];
        SFVoice &v = cs.voices[osc];

        v.sampleData = (short *)(sf.dwStart);
        v.sampleSize = (int)(sf.dwEnd);
        v.sampleRate = sf.dwSampleRate;

        // Root note from SF2 sample header
        twoByteUnion tbu;
        tbu.wVal = sf.shOrigKeyAndCorr;
        v.rootNote = tbu.byVals.by1;

        // Loop points
        v.looped = ((sf.shSampleModes & 0x1) != 0);
        v.loopStart = v.looped ? (int)(sf.dwStartloop) : -1;
        v.loopEnd = (int)(sf.dwEndloop);
        v.velocity = velocity;

        if (!v.sampleData || v.sampleSize <= 0) {
            v.active = false;
            continue;
        }

        v.active = true;

        // Compute playback speed based on sample rate and pitch
        v.speed = (double)v.sampleRate / (double)driverRate;
        v.position = 0.0;

        // SF2 tuning
        v.coarseTune = sf.shCoarseTune;
        v.fineTune = sf.shFineTune;
        v.scaleTuning = sf.shScaleTuning;
        if (v.scaleTuning <= 0) v.scaleTuning = 100;

        // SF2 initial attenuation (centibels)
        v.initialAttenuation = centibelToGain(sf.shInstVol);

        // SF2 pan: -500..500 → -50..50 percentage
        v.sfPan = sf.shPanEffectsSend / 10.0f;

        // Volume Envelope from SF2 data
        double sr = (double)v.sampleRate;
        v.volEnvDelay = timecentsToSamples(sf.shDelayVolEnv, sr);
        double attackSamples = timecentsToSamples(sf.shAttackVolEnv, sr);
        v.volEnvAttack = (attackSamples > 0) ? (1.0 / attackSamples) : 1.0;
        v.volEnvHold = timecentsToSamples(sf.shHoldVolEnv, sr);
        double decaySamples = timecentsToSamples(sf.shDecayVolEnv, sr);
        // SF2 sustain is in centibels of attenuation (0 = full, 1000 = -100dB)
        double sustainCB = sf.shSustainVolEnv;
        if (sustainCB < 0) sustainCB = 0;
        if (sustainCB > 1000) sustainCB = 1000;
        v.volEnvSustain = pow(10.0, -sustainCB / 200.0);
        // Decay rate: from 1.0 down to sustain level in decaySamples
        if (decaySamples > 0 && v.volEnvSustain < 1.0) {
            v.volEnvDecay = (1.0 - v.volEnvSustain) / decaySamples;
        } else {
            v.volEnvDecay = 0.0;
        }
        double releaseSamples = timecentsToSamples(sf.shReleaseVolEnv, sr);
        v.volEnvRelease = (releaseSamples > 0) ? (v.volEnvSustain / releaseSamples) : 1.0;

        // Start the envelope
        if (v.volEnvDelay > 0) {
            v.volEnvStage = SF_ENV_DELAY;
            v.volEnvLevel = 0.0;
            v.volEnvCounter = v.volEnvDelay;
        } else {
            v.volEnvStage = SF_ENV_ATTACK;
            v.volEnvLevel = 0.0;
            v.volEnvCounter = 0;
        }

        // Apply pitch including SF2 tuning
        int pitchOffset = (int)note - v.rootNote;
        double totalCents = (double)pitchOffset * (double)v.scaleTuning
                            + (double)v.coarseTune * 100.0
                            + (double)v.fineTune;
        v.speed *= pow(2.0, totalCents / 1200.0);

        cs.voiceCount++;
    }

    // Deactivate any remaining voice slots
    for (int i = cs.voiceCount; i < SF_MAX_LAYERS; i++) {
        cs.voices[i].active = false;
    }

    if (cs.voiceCount == 0) {
        cs.finished = true;
        return false;
    }

    return true;
}

void SoundFontInstrument::Stop(int channel) {
    if (channel < 0 || channel >= SONG_CHANNEL_COUNT) return;

    SFChannelState &cs = channels_[channel];

    // Transition active voices to release stage
    for (int i = 0; i < cs.voiceCount; i++) {
        SFVoice &v = cs.voices[i];
        if (v.active && v.volEnvStage != SF_ENV_RELEASE && v.volEnvStage != SF_ENV_DONE) {
            v.volEnvStage = SF_ENV_RELEASE;
        }
    }
}

void SoundFontInstrument::OnStart() {
    // Nothing needed on song start
}

void SoundFontInstrument::processVolumeEnvelope(SFVoice &voice) {
    switch (voice.volEnvStage) {
        case SF_ENV_IDLE:
            voice.volEnvLevel = 0.0;
            break;

        case SF_ENV_DELAY:
            voice.volEnvLevel = 0.0;
            voice.volEnvCounter -= 1.0;
            if (voice.volEnvCounter <= 0) {
                voice.volEnvStage = SF_ENV_ATTACK;
            }
            break;

        case SF_ENV_ATTACK:
            voice.volEnvLevel += voice.volEnvAttack;
            if (voice.volEnvLevel >= 1.0) {
                voice.volEnvLevel = 1.0;
                if (voice.volEnvHold > 0) {
                    voice.volEnvStage = SF_ENV_HOLD;
                    voice.volEnvCounter = voice.volEnvHold;
                } else {
                    voice.volEnvStage = (voice.volEnvDecay > 0) ? SF_ENV_DECAY : SF_ENV_SUSTAIN;
                }
            }
            break;

        case SF_ENV_HOLD:
            voice.volEnvLevel = 1.0;
            voice.volEnvCounter -= 1.0;
            if (voice.volEnvCounter <= 0) {
                voice.volEnvStage = (voice.volEnvDecay > 0) ? SF_ENV_DECAY : SF_ENV_SUSTAIN;
            }
            break;

        case SF_ENV_DECAY:
            voice.volEnvLevel -= voice.volEnvDecay;
            if (voice.volEnvLevel <= voice.volEnvSustain) {
                voice.volEnvLevel = voice.volEnvSustain;
                voice.volEnvStage = SF_ENV_SUSTAIN;
            }
            break;

        case SF_ENV_SUSTAIN:
            // Hold at sustain level until note off (→ release)
            voice.volEnvLevel = voice.volEnvSustain;
            break;

        case SF_ENV_RELEASE:
            voice.volEnvLevel -= voice.volEnvRelease;
            if (voice.volEnvLevel <= 0.0) {
                voice.volEnvLevel = 0.0;
                voice.volEnvStage = SF_ENV_DONE;
                voice.active = false;
            }
            break;

        case SF_ENV_DONE:
            voice.volEnvLevel = 0.0;
            voice.active = false;
            break;
    }
}

bool SoundFontInstrument::Render(int channel, fixed *buffer, int size, bool updateTick) {
    if (channel < 0 || channel >= SONG_CHANNEL_COUNT) return false;

    SFChannelState &cs = channels_[channel];
    if (cs.finished) return false;

    // Clear output buffer (interleaved stereo: size*2 entries)
    memset(buffer, 0, size * 2 * sizeof(fixed));

    // Check if any voice is still active
    bool anyActive = false;

    // User volume: 0-255 → fixed-point scale matching SampleInstrument
    // SampleInstrument: volfactor = fp_mul(i2fp(volume), fl2fp(1/255))
    // which is volume/255 in fixed-point. We replicate that exactly.
    int userVolInt = volume_->GetInt();
    fixed volScale = fl2fp(1.0f / 255.0f);
    fixed volFactor = fp_mul(i2fp(userVolInt), volScale);

    // User pan: 0-254, 0x7F = center
    // Use the same panlaw table as SampleInstrument
    int userPan = pan_->GetInt();
    if (userPan < 0) userPan = 0;
    if (userPan > 254) userPan = 254;

    extern fixed panlaw[];
    fixed fixedPanL = panlaw[userPan];
    fixed fixedPanR = panlaw[254 - userPan];

    // Compute LFO modulation factors (per-buffer, updated per sample)
    float tremoloPhase = cs.tremolo.phase;
    float vibratoPhase = cs.vibrato.phase;

    for (int vi = 0; vi < cs.voiceCount; vi++) {
        SFVoice &v = cs.voices[vi];
        if (!v.active) continue;

        // Voice-level gain from SF2 attenuation → fixed-point
        fixed sfGain = fl2fp(v.initialAttenuation);

        // SF2 pan → left/right multipliers in fixed-point
        float sfPanR_f, sfPanL_f;
        if (v.sfPan >= 0) {
            sfPanR_f = 1.0f;
            sfPanL_f = 1.0f - (v.sfPan / 50.0f);
        } else {
            sfPanL_f = 1.0f;
            sfPanR_f = 1.0f + (v.sfPan / 50.0f);
        }
        if (sfPanL_f < 0.0f) sfPanL_f = 0.0f;
        if (sfPanR_f < 0.0f) sfPanR_f = 0.0f;
        fixed sfPanLfp = fl2fp(sfPanL_f);
        fixed sfPanRfp = fl2fp(sfPanR_f);

        short *samples = v.sampleData;

        // Reset LFO phases for each voice to keep them in sync
        float tPhase = tremoloPhase;
        float vPhase = vibratoPhase;

        for (int i = 0; i < size; i++) {
            // Process envelope
            processVolumeEnvelope(v);
            if (!v.active) break;

            fixed envGain = fl2fp((float)v.volEnvLevel);

            // Compute tremolo volume multiplier
            fixed tremoloMul = FP_ONE;
            if (cs.tremolo.active) {
                float sine = sinf(tPhase * 2.0f * 3.14159265f / 256.0f);
                // depth=1.0 means volume swings from 0 to 2x (±100%)
                float volMul = 1.0f + sine * cs.tremolo.depth;
                if (volMul < 0.0f) volMul = 0.0f;
                tremoloMul = fl2fp(volMul);
                tPhase += cs.tremolo.speed;
                if (tPhase >= 256.0f) tPhase -= 256.0f;
            }

            // Compute vibrato pitch multiplier
            double speedMul = 1.0;
            if (cs.vibrato.active) {
                float sine = sinf(vPhase * 2.0f * 3.14159265f / 256.0f);
                // depth=1.0 means ±4 semitones
                speedMul = pow(2.0, (double)(sine * cs.vibrato.depth * 4.0f) / 12.0);
                vPhase += cs.vibrato.speed;
                if (vPhase >= 256.0f) vPhase -= 256.0f;
            }

            // Read sample with linear interpolation in fixed-point
            int pos = (int)v.position;
            fixed fpFrac = fl2fp((float)(v.position - pos));

            fixed s0 = 0, s1 = 0;
            if (pos >= 0 && pos < v.sampleSize) {
                s0 = i2fp(samples[pos]);
            }
            if (pos + 1 < v.sampleSize) {
                s1 = i2fp(samples[pos + 1]);
            } else {
                s1 = s0;
            }
            // Linear interpolation: sample = s0 + (s1-s0)*frac
            fixed sample = fp_add(s0, fp_mul(fp_sub(s1, s0), fpFrac));

            // Apply envelope
            sample = fp_mul(sample, envGain);

            // Apply SF2 attenuation
            sample = fp_mul(sample, sfGain);

            // Apply user volume (same scale as SampleInstrument)
            sample = fp_mul(sample, volFactor);

            // Apply tremolo modulation
            sample = fp_mul(sample, tremoloMul);

            // Apply SF2 pan + user pan
            fixed outL = fp_mul(fp_mul(sample, sfPanLfp), fixedPanL);
            fixed outR = fp_mul(fp_mul(sample, sfPanRfp), fixedPanR);

            // Accumulate to interleaved stereo buffer (additive for multi-voice)
            buffer[i * 2]     = fp_add(buffer[i * 2], outL);
            buffer[i * 2 + 1] = fp_add(buffer[i * 2 + 1], outR);

            // Advance position with vibrato pitch modulation
            v.position += v.speed * speedMul;

            // Handle loop / end of sample
            if (v.looped && v.loopEnd > v.loopStart && v.loopStart >= 0) {
                while (v.position >= (double)v.loopEnd) {
                    v.position -= (double)(v.loopEnd - v.loopStart);
                }
            } else {
                // One-shot: stop when reaching end
                if (v.position >= (double)v.sampleSize) {
                    v.active = false;
                    break;
                }
            }
        }

        if (v.active) anyActive = true;
    }

    // Save LFO phases back (use the last voice's phase progression)
    if (cs.tremolo.active) {
        cs.tremolo.phase = tremoloPhase + cs.tremolo.speed * size;
        while (cs.tremolo.phase >= 256.0f) cs.tremolo.phase -= 256.0f;
    }
    if (cs.vibrato.active) {
        cs.vibrato.phase = vibratoPhase + cs.vibrato.speed * size;
        while (cs.vibrato.phase >= 256.0f) cs.vibrato.phase -= 256.0f;
    }

    // Apply de-click fade out if requested
    if (cs.fadeOutSamples > 0) {
        for (int i = 0; i < size && cs.fadeOutSamples > 0; i++) {
            fixed fade = fl2fp((float)cs.fadeOutSamples / (float)cs.fadeOutTotal);
            buffer[i * 2]     = fp_mul(buffer[i * 2], fade);
            buffer[i * 2 + 1] = fp_mul(buffer[i * 2 + 1], fade);
            cs.fadeOutSamples--;
        }
        if (cs.fadeOutSamples <= 0) {
            anyActive = false;
        }
    }

    if (!anyActive) {
        cs.finished = true;
        return false;
    }

    return true;
}

bool SoundFontInstrument::IsInitialized() {
    return true;
}

bool SoundFontInstrument::IsEmpty() {
    return (bankID_ < 0 || currentPreset_ < 0);
}

const char *SoundFontInstrument::GetName() {
    if (IsEmpty()) return "-- no sf2 --";
    return sf2Name_;
}

void SoundFontInstrument::ProcessCommand(int channel, FourCC cc, ushort value) {
    if (channel < 0 || channel >= SONG_CHANNEL_COUNT) return;

    SFChannelState &cs = channels_[channel];
    float driverRate = (float)Audio::GetInstance()->GetSampleRate();
    // k-rate equivalent: we update LFOs every KRATE_SAMPLE_COUNT samples
    // speed byte maps to phase increments per k-rate tick, then convert to per-sample
    float kRateDivisor = (float)KRATE_SAMPLE_COUNT;

    switch (cc) {
        case I_CMD_KILL:
            // Instant kill (with fade)
            cs.fadeOutSamples = SF_DECLICK_FADE;
            cs.fadeOutTotal = SF_DECLICK_FADE;
            break;
        case I_CMD_VOLM:
            // Set volume for this channel's voices
            break;
        case I_CMD_TRML:
            {
                unsigned char speed = (value >> 8) & 0xFF;
                unsigned char depth = value & 0xFF;
                cs.tremolo.speed = (speed * 2.0f) / kRateDivisor; // phase inc per sample
                cs.tremolo.depth = depth / 255.0f;
                cs.tremolo.active = true;
            }
            break;
        case I_CMD_VIBR:
            {
                unsigned char speed = (value >> 8) & 0xFF;
                unsigned char depth = value & 0xFF;
                cs.vibrato.speed = (speed * 2.0f) / kRateDivisor; // phase inc per sample
                cs.vibrato.depth = depth / 255.0f;
                cs.vibrato.active = true;
            }
            break;
        case I_CMD_LFOF:
            {
                unsigned char speed = (value >> 8) & 0xFF;
                unsigned char depth = value & 0xFF;
                cs.lfoFilter.speed = (speed * 2.0f) / kRateDivisor;
                cs.lfoFilter.depth = depth / 255.0f;
                cs.lfoFilter.active = true;
            }
            break;
        default:
            break;
    }
}

void SoundFontInstrument::Purge() {
    // Stop all channels
    for (int c = 0; c < SONG_CHANNEL_COUNT; c++) {
        channels_[c].finished = true;
        for (int v = 0; v < SF_MAX_LAYERS; v++) {
            channels_[c].voices[v].active = false;
        }
    }

    delete presetSource_;
    presetSource_ = nullptr;

    // Unload the SF2 bank
    if (bankID_ >= 0) {
        SoundFontManager::GetInstance()->UnloadBank(bankID_);
    }

    sf2Path_[0] = '\0';
    strcpy(sf2Name_, "SF2");
    bankID_ = -1;
    presetCount_ = 0;
    currentPreset_ = -1;
    presetNames_.clear();
}

int SoundFontInstrument::GetTable() {
    return table_->GetInt();
}

bool SoundFontInstrument::GetTableAutomation() {
    return tableAuto_->GetInt() != 0;
}

void SoundFontInstrument::GetTableState(TableSaveState &state) {
    state = tableState_;
}

void SoundFontInstrument::SetTableState(TableSaveState &state) {
    tableState_ = state;
}

// ─────────────────────────────────────────────────────────────────────────────
//  I_Observer
// ─────────────────────────────────────────────────────────────────────────────

void SoundFontInstrument::Update(Observable &o, I_ObservableData *d) {
    // Placeholder for observed variable changes
}

// ─────────────────────────────────────────────────────────────────────────────
//  SoundFont-specific methods
// ─────────────────────────────────────────────────────────────────────────────

void SoundFontInstrument::SetSF2File(const char *path) {
    // Clean up previous state
    delete presetSource_;
    presetSource_ = nullptr;
    presetNames_.clear();
    presetCount_ = 0;
    currentPreset_ = -1;

    // Unload previous bank if any
    if (bankID_ >= 0) {
        SoundFontManager::GetInstance()->UnloadBank(bankID_);
        bankID_ = -1;
    }

    strncpy(sf2Path_, path, sizeof(sf2Path_) - 1);
    sf2Path_[sizeof(sf2Path_) - 1] = '\0';

    // Load the SF2 bank
    bankID_ = SoundFontManager::GetInstance()->LoadBank(path);
    if (bankID_ < 0) {
        Trace::Error("SoundFontInstrument: Failed to load SF2: %s", path);
        sf2Path_[0] = '\0';
        return;
    }

    // Extract filename for display
    const char *slash = strrchr(path, '/');
    if (!slash) slash = strrchr(path, '\\');
    const char *fname = slash ? slash + 1 : path;
    strncpy(sf2Name_, fname, sizeof(sf2Name_) - 1);
    sf2Name_[sizeof(sf2Name_) - 1] = '\0';
    // Remove .sf2 extension from display name
    char *dot = strrchr(sf2Name_, '.');
    if (dot) *dot = '\0';

    // Enumerate presets
    loadPresets();

    // Auto-select first preset if available
    if (presetCount_ > 0) {
        SelectPreset(0);
    }

    Trace::Debug("SoundFontInstrument: Loaded %s with %d presets", sf2Name_, presetCount_);
}

void SoundFontInstrument::loadPresets() {
    presetNames_.clear();
    presetCount_ = 0;

    if (bankID_ < 0) return;

    WORD count = 0;
    SFPRESETHDRPTR pHeaders = sfGetPresetHdrs(bankID_, &count);
    if (!pHeaders || count == 0) return;

    for (int i = 0; i < count; i++) {
        presetNames_.push_back(std::string(pHeaders[i].achPresetName));
    }
    presetCount_ = count;
}

const char *SoundFontInstrument::GetPresetName(int index) const {
    if (index < 0 || index >= presetCount_) return "---";
    return presetNames_[index].c_str();
}

void SoundFontInstrument::SelectPreset(int presetIndex) {
    if (bankID_ < 0 || presetIndex < 0 || presetIndex >= presetCount_) return;

    Trace::Log("SF2", "SelectPreset(%d) [was %d, bank %d, count %d]",
                 presetIndex, currentPreset_, bankID_, presetCount_);

    // Stop all active voices
    for (int c = 0; c < SONG_CHANNEL_COUNT; c++) {
        channels_[c].finished = true;
        for (int v = 0; v < SF_MAX_LAYERS; v++) {
            channels_[c].voices[v].active = false;
        }
    }

    delete presetSource_;
    presetSource_ = new SoundFontPreset(bankID_, presetIndex);
    currentPreset_ = presetIndex;

    // Keep the preset variable in sync
    if (presetVar_->GetInt() != presetIndex) {
        presetVar_->SetInt(presetIndex);
    }

    Trace::Debug("SoundFontInstrument: Selected preset %d: %s", presetIndex, GetPresetName(presetIndex));
}

void SoundFontInstrument::StorePendingVariable(const char *name, const char *value) {
    if (name && value) {
        pendingParamValues_[name] = value;
    }
}

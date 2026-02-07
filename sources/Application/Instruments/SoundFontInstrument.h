#ifndef _SOUNDFONT_INSTRUMENT_H_
#define _SOUNDFONT_INSTRUMENT_H_

#include "I_Instrument.h"
#include "SoundFontPreset.h"
#include "SoundFontManager.h"
#include "Application/Model/Song.h"
#include "Foundation/Observable.h"
#include "Foundation/Variables/WatchedVariable.h"
#include "Application/Utils/fixed.h"
#include <string>
#include <vector>
#include <map>
#include <cmath>

// Instrument parameter FourCC IDs
#define SFIP_VOLUME     MAKE_FOURCC('S','V','O','L')
#define SFIP_PAN        MAKE_FOURCC('S','P','A','N')
#define SFIP_TABLE      MAKE_FOURCC('S','T','B','L')
#define SFIP_TABLEAUTO  MAKE_FOURCC('S','T','B','A')
#define SFIP_PRESET     MAKE_FOURCC('S','F','P','R')
#define SFIP_BANK       MAKE_FOURCC('S','F','B','K')
#define SFIP_SFFILE     MAKE_FOURCC('S','F','F','L')

// Max SoundFont instrument slots
#define MAX_SOUNDFONTINSTRUMENT_COUNT 0x10

// Maximum polyphony per channel voice
#define SF_MAX_LAYERS 4

// ADSR envelope stages
enum SFEnvStage {
    SF_ENV_IDLE = 0,
    SF_ENV_DELAY,
    SF_ENV_ATTACK,
    SF_ENV_HOLD,
    SF_ENV_DECAY,
    SF_ENV_SUSTAIN,
    SF_ENV_RELEASE,
    SF_ENV_DONE
};

// Per-oscillator voice state (one SoundFont preset can have up to 4 layers)
struct SFVoice {
    bool active;
    short *sampleData;
    int sampleSize;       // in samples
    int sampleRate;
    int rootNote;
    int loopStart;        // in samples, -1 if no loop
    int loopEnd;
    bool looped;

    double position;       // fractional sample position
    double speed;          // playback speed factor

    // Volume envelope (DAHDSR)
    SFEnvStage volEnvStage;
    double volEnvLevel;    // 0.0 .. 1.0
    double volEnvDelay;    // in samples
    double volEnvAttack;   // rate per sample
    double volEnvHold;     // in samples
    double volEnvDecay;    // rate per sample
    double volEnvSustain;  // level 0..1
    double volEnvRelease;  // rate per sample
    double volEnvCounter;  // generic counter

    // SF2 initial attenuation (centibels)
    float initialAttenuation;

    // SF2 pan (-500 = full left, 0 = center, 500 = full right)
    float sfPan;

    // SF2 tuning
    int coarseTune;
    int fineTune;
    int scaleTuning;  // cents/key, default 100

    // Velocity
    int velocity;
};

// Per-channel rendering state
struct SFChannelState {
    bool finished;
    unsigned char midiNote;
    SFVoice voices[SF_MAX_LAYERS];
    int voiceCount;

    // Fade out for click prevention
    int fadeOutSamples;
    int fadeOutTotal;
};

class SoundFontInstrument : public I_Instrument, public I_Observer {
public:
    SoundFontInstrument();
    virtual ~SoundFontInstrument();

    // I_Instrument implementation
    virtual bool Init();
    virtual bool Start(int channel, unsigned char note, bool retrigger = true);
    virtual void Stop(int channel);
    virtual void OnStart();
    virtual bool Render(int channel, fixed *buffer, int size, bool updateTick);
    virtual bool IsInitialized();
    virtual bool IsEmpty();
    virtual InstrumentType GetType() { return IT_SOUNDFONT; }
    virtual const char *GetName();
    virtual void ProcessCommand(int channel, FourCC cc, ushort value);
    virtual void Purge();
    virtual int GetTable();
    virtual bool GetTableAutomation();
    virtual void GetTableState(TableSaveState &state);
    virtual void SetTableState(TableSaveState &state);

    // I_Observer
    virtual void Update(Observable &o, I_ObservableData *d);

    // SoundFont-specific
    void SetSF2File(const char *path);
    const char *GetSF2Path() const { return sf2Path_; }
    int GetPresetCount() const { return presetCount_; }
    const char *GetPresetName(int index) const;
    void SelectPreset(int presetIndex);
    int GetCurrentPreset() const { return currentPreset_; }

    // Persistence helpers
    void StorePendingVariable(const char *name, const char *value);

private:
    // Convert SF2 timecents to samples: timecents → seconds → samples
    double timecentsToSamples(short tc, double sampleRate);
    // Convert SF2 centibels to linear gain
    float centibelToGain(short cb);
    // Process the volume envelope for a voice
    void processVolumeEnvelope(SFVoice &voice);
    // Load SF2 presets from the file
    void loadPresets();

    // SF2 file and preset state
    char sf2Path_[256];
    char sf2Name_[80];
    sfBankID bankID_;
    int presetCount_;
    int currentPreset_;

    // The SoundFontPreset source (created per-preset selection)
    SoundFontPreset *presetSource_;

    // Per-channel state
    SFChannelState channels_[SONG_CHANNEL_COUNT];

    // Variables for UI
    Variable *volume_;
    Variable *pan_;
    Variable *table_;
    Variable *tableAuto_;
    WatchedVariable *presetVar_;

    TableSaveState tableState_;

    // Preset names for display
    std::vector<std::string> presetNames_;

    // Pending variables from project restore
    std::map<std::string, std::string> pendingParamValues_;
};

#endif

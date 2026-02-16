#ifndef _AUDIO_IN_INSTRUMENT_H_
#define _AUDIO_IN_INSTRUMENT_H_

#include "I_Instrument.h"
#include "I_Effect.h"
#include "Application/Model/Song.h"
#include "Services/Midi/MidiService.h"
#include "Services/Audio/AudioModule.h"
#include "Application/Utils/fixed.h"
#include "System/Process/SysMutex.h"
#include <SDL2/SDL.h>
#include <string>
#include <vector>

// Variable FourCC IDs for AudioIn instrument parameters
#define AIP_INPUTDEVICE  MAKE_FOURCC('A','I','D','V')
#define AIP_VOLUME       MAKE_FOURCC('A','I','V','L')
#define AIP_PAN          MAKE_FOURCC('A','I','P','N')
#define AIP_MIDIDEVICE   MAKE_FOURCC('A','I','M','D')
#define AIP_MIDICHANNEL  MAKE_FOURCC('A','I','M','C')
#define AIP_NOTELENGTH   MAKE_FOURCC('A','I','L','N')
#define AIP_TABLE        MAKE_FOURCC('A','I','T','B')
#define AIP_TABLEAUTO    MAKE_FOURCC('A','I','T','A')
#define AIP_TRANSPOSE    MAKE_FOURCC('A','I','T','R')

class MixBus;

class AudioInInstrument : public I_Instrument {
public:
    AudioInInstrument();
    virtual ~AudioInInstrument();

    virtual bool Init();

    // Called when user explicitly creates/selects this instrument type.
    // Opens the capture device and registers on the MixBus so audio flows.
    void Activate();

    virtual bool Start(int channel, unsigned char note, bool retrigger = true);
    virtual void Stop(int channel);

    // I_Instrument::Render handles MIDI timing only; returns false (no audio
    // contribution through PlayerChannel). Live audio flows via CaptureModule
    // directly on the MixBus.
    virtual bool Render(int channel, fixed *buffer, int size, bool updateTick);
    virtual void ProcessCommand(int channel, FourCC cc, ushort value);

    virtual bool IsInitialized();

    virtual bool IsEmpty() { return false; }

    virtual InstrumentType GetType() { return IT_AUDIOIN; }

    virtual const char *GetName();

    virtual void OnStart();

    virtual void Purge() {}

    virtual int GetTable();
    virtual bool GetTableAutomation();
    virtual void GetTableState(TableSaveState &state);
    virtual void SetTableState(TableSaveState &state);

    // Input device list accessors for the UI
    int GetInputDeviceCount();
    const char *GetInputDeviceName(int index);

    // MIDI output device list accessors for the UI
    int GetMidiDeviceCount();
    const char *GetMidiDeviceName(int index);

    // Effect routing — called from Player FXSN handler
    void SetEffect(I_Effect *effect, int wetDry);
    void ClearEffect();

private:
    // Inner AudioModule that lives on a MixBus and renders captured audio
    // every audio callback, regardless of note triggers.
    class CaptureModule : public AudioModule {
    public:
        CaptureModule(AudioInInstrument &owner) : owner_(owner) {}
        bool Render(fixed *buffer, int samplecount) override;
    private:
        AudioInInstrument &owner_;
    };
    friend class CaptureModule;

    void probeDeviceCapabilities();
    void openCaptureDevice();
    void closeCaptureDevice();
    void enumerateInputDevices();
    bool renderCapture(fixed *buffer, int samplecount);
    void registerOnBus();
    void unregisterFromBus();
    void sendMidi(MidiMessage &msg);
    void sendMidiToDevice(MidiMessage &msg, int deviceIndex);

    CaptureModule captureModule_;
    MixBus *currentBus_;

    char name_[40];
    int lastNote_[SONG_CHANNEL_COUNT];
    int lastMidiChannel_[SONG_CHANNEL_COUNT];
    int lastMidiDevice_[SONG_CHANNEL_COUNT];
    int remainingTicks_[SONG_CHANNEL_COUNT];
    bool playing_[SONG_CHANNEL_COUNT];
    bool first_[SONG_CHANNEL_COUNT];
    unsigned char velocity_[SONG_CHANNEL_COUNT];
    TableSaveState tableState_;

    // SDL2 audio capture state
    SDL_AudioDeviceID captureDeviceId_;
    SDL_AudioSpec captureSpec_;
    int currentInputDevice_;     // Currently opened device index
    int nativeRate_;             // Probed native sample rate
    int nativeChannels_;         // Probed native channel count
    std::vector<std::string> inputDevices_;
    SysMutex captureMutex_;

    // Capture buffer for converting F32 -> fixed
    float captureBuffer_[4096];

    // Effect applied to captured audio (set via FXSN command)
    I_Effect *activeEffect_;
    int effectWetDry_;

    static MidiService *svc_;
};

#endif

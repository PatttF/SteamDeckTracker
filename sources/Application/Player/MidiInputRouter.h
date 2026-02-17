#ifndef _MIDI_INPUT_ROUTER_H_
#define _MIDI_INPUT_ROUTER_H_

#include "Foundation/Observable.h"
#include "Foundation/T_Singleton.h"

class Project;
class PlayerMixer;
class MidiService;

// Routes incoming MIDI note events to instruments based on their
// IMDI (device) and IMIC (channel) variable settings.
// Uses a small pool of dedicated "live" mixer channels so MIDI
// input doesn't conflict with tracker sequencer playback.

class MidiInputRouter : public I_Observer {
public:
    MidiInputRouter();
    ~MidiInputRouter();

    void Init(Project *project, PlayerMixer *mixer);
    void Close();

    // Re-subscribe to all MIDI input devices (call after device refresh)
    void RefreshDevices();

protected:
    void Update(Observable &o, I_ObservableData *d) override;

private:
    Project *project_;
    PlayerMixer *mixer_;
    bool initialized_;

    // Live channel pool: use tracker channels 8-15 for live MIDI input.
    // Each active note maps to one channel. On note-off, channel is freed.
    static const int LIVE_CH_BASE = 8;
    static const int LIVE_CH_COUNT = 8;

    struct LiveNote {
        int instrument;     // instrument index in bank (-1 = free)
        unsigned char note; // MIDI note number
    };
    LiveNote liveNotes_[LIVE_CH_COUNT];

    int allocLiveChannel();
    int findLiveChannel(int instrIdx, unsigned char note);
    void freeLiveChannel(int slot);
};

#endif

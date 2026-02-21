#ifndef _MIDI_INPUT_ROUTER_H_
#define _MIDI_INPUT_ROUTER_H_

#include "Foundation/Observable.h"
#include "Foundation/T_Singleton.h"

class Project;
class PlayerMixer;
class MidiService;
class I_Instrument;

// Routes incoming MIDI note events to instruments based on their
// IMDI (device) and IMIC (channel) variable settings.
// Uses a small pool of dedicated "live" mixer channels so MIDI
// input doesn't conflict with tracker sequencer playback.
//
// Plugin instruments (VST3/LV2) use a SINGLE channel per instrument
// with all polyphony handled internally by the plugin.  Sample-based
// instruments use one channel per active note.

class MidiInputRouter : public I_Observer {
public:
    MidiInputRouter();
    ~MidiInputRouter();

    void Init(Project *project, PlayerMixer *mixer);
    void Close();

    // Re-subscribe to all MIDI input devices (call after device refresh)
    void RefreshDevices();

    // Clear any router state (live/plugin slots) for a specific instrument
    // — call after an instrument is type-switched or deleted so the router
    // doesn't hold stale references to the old instrument index.
    void ClearInstrumentRouting(int instrIdx);

protected:
    void Update(Observable &o, I_ObservableData *d) override;

private:
    Project *project_;
    PlayerMixer *mixer_;
    bool initialized_;

    // Live channel pool: use tracker channels 8-15 for live MIDI input.
    static const int LIVE_CH_BASE = 8;
    static const int LIVE_CH_COUNT = 8;

    // --- Per-note channels (Sample/SoundFont instruments) ---
    struct LiveNote {
        int instrument;     // instrument index in bank (-1 = free)
        unsigned char note; // MIDI note number
    };
    LiveNote liveNotes_[LIVE_CH_COUNT];

    int allocLiveChannel();
    int findLiveChannel(int instrIdx, unsigned char note);
    void freeLiveChannel(int slot);

    // --- Single-channel polyphony (VST3/LV2 plugin instruments) ---
    // One mixer channel per instrument; all notes routed as MIDI events.
    static const int MAX_PLUGIN_SLOTS = 8; // max simultaneous plugin instruments
    struct PluginSlot {
        int instrument;   // instrument index (-1 = free)
        int slot;         // live channel slot index
        int noteCount;    // number of currently-held notes
    };
    PluginSlot pluginSlots_[MAX_PLUGIN_SLOTS];

    bool isPluginInstrument(I_Instrument *instr);
    int findPluginSlot(int instrIdx);
    int allocPluginSlot(int instrIdx);
    void freePluginSlot(int instrIdx);
};

#endif

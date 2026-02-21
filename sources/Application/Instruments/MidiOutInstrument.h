#ifndef _MIDI_OUT_INSTRUMENT_H_
#define _MIDI_OUT_INSTRUMENT_H_

#include "I_Instrument.h"
#include "Application/Model/Song.h"
#include "Services/Midi/MidiService.h"
#include "Application/Player/SyncMaster.h"

#define MIDI_NOTE_ON 0x90
#define MIDI_NOTE_OFF 0x80
#define MIDI_CC 0xB0
#define MIDI_PRG 0xC0
#define MIDI_PB 0xE0
#define MIDI_PAT 0xA0
#define MIDI_CAT 0xD0

#define MOIP_DEVICE     MAKE_FOURCC('M','O','D','V')
#define MOIP_CHANNEL    MAKE_FOURCC('M','O','C','H')
#define MOIP_NOTELENGTH MAKE_FOURCC('M','O','L','N')
#define MOIP_TABLE      MAKE_FOURCC('M','O','T','B')
#define MOIP_TABLEAUTO  MAKE_FOURCC('M','O','T','A')
#define MOIP_TRANSPOSE  MAKE_FOURCC('M','O','T','R')
#define MOIP_CLOCK      MAKE_FOURCC('M','O','C','K')
#define MOIP_TRANSPORT  MAKE_FOURCC('M','O','T','P')

class MidiOutInstrument : public I_Instrument {
public:
    MidiOutInstrument();
    virtual ~MidiOutInstrument();

    virtual bool Init();

    virtual bool Start(int channel, unsigned char note, bool retrigger = true);
    virtual void Stop(int channel);

    virtual bool Render(int channel, fixed *buffer, int size, bool updateTick);
    virtual void ProcessCommand(int channel, FourCC cc, ushort value);

    virtual bool IsInitialized();

    virtual bool IsEmpty() { return false; }

    virtual InstrumentType GetType() { return IT_MIDIOUT; }

    virtual const char *GetName();

    virtual void OnStart();

    virtual void Purge() {}

    virtual int GetTable();
    virtual bool GetTableAutomation();
    virtual void GetTableState(TableSaveState &state);
    virtual void SetTableState(TableSaveState &state);

    // Device list accessors for the UI
    int GetDeviceCount();
    const char *GetDeviceName(int index);

private:
    void sendMidi(MidiMessage &msg);
    void sendMidiToDevice(MidiMessage &msg, int deviceIndex);

    char name_[40];
    int lastNote_[SONG_CHANNEL_COUNT];
    int lastChannel_[SONG_CHANNEL_COUNT];
    int lastDevice_[SONG_CHANNEL_COUNT];
    int remainingTicks_[SONG_CHANNEL_COUNT];
    bool playing_[SONG_CHANNEL_COUNT];
    bool retrig_[SONG_CHANNEL_COUNT];
    int retrigLoop_[SONG_CHANNEL_COUNT];
    unsigned char velocity_[SONG_CHANNEL_COUNT];
    bool transportRunning_;
    TableSaveState tableState_;
    bool first_[SONG_CHANNEL_COUNT];

    static MidiService *svc_;
};

#endif

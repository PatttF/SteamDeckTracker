#include "MidiInputRouter.h"
#include "PlayerMixer.h"
#include "Application/Model/Project.h"
#include "Application/Instruments/InstrumentBank.h"
#include "Application/Instruments/I_Instrument.h"
#include "Services/Midi/MidiService.h"
#include "Services/Midi/MidiInDevice.h"
#include "Services/Midi/MidiMessage.h"
#include "Foundation/Variables/Variable.h"
#include "System/Console/Trace.h"

// FourCC IDs matching InstrumentView.h
#define IMDI MAKE_FOURCC('I','M','D','I')
#define IMIC MAKE_FOURCC('I','M','I','C')

MidiInputRouter::MidiInputRouter()
    : project_(nullptr), mixer_(nullptr), initialized_(false) {
    for (int i = 0; i < LIVE_CH_COUNT; i++) {
        liveNotes_[i].instrument = -1;
        liveNotes_[i].note = 0;
    }
}

MidiInputRouter::~MidiInputRouter() {
    Close();
}

void MidiInputRouter::Init(Project *project, PlayerMixer *mixer) {
    project_ = project;
    mixer_ = mixer;
    initialized_ = true;
    RefreshDevices();
}

void MidiInputRouter::Close() {
    if (!initialized_) return;
    // Unsubscribe and stop all MIDI input devices
    MidiService *svc = MidiService::GetInstance();
    if (svc) {
        IteratorPtr<MidiInDevice> it(svc->GetInIterator());
        for (it->Begin(); !it->IsDone(); it->Next()) {
            MidiInDevice &dev = it->CurrentItem();
            dev.RemoveObserver(*this);
            if (dev.IsRunning()) {
                dev.Stop();
                dev.Close();
            }
        }
    }
    initialized_ = false;
}

void MidiInputRouter::RefreshDevices() {
    if (!initialized_) return;
    MidiService *svc = MidiService::GetInstance();
    if (!svc) return;

    // Init, start and subscribe to all input devices
    IteratorPtr<MidiInDevice> it(svc->GetInIterator());
    for (it->Begin(); !it->IsDone(); it->Next()) {
        MidiInDevice &dev = it->CurrentItem();
        dev.RemoveObserver(*this);
        if (!dev.IsRunning()) {
            dev.Init();
            dev.Start();
        }
        dev.AddObserver(*this);
    }
}

int MidiInputRouter::allocLiveChannel() {
    for (int i = 0; i < LIVE_CH_COUNT; i++) {
        if (liveNotes_[i].instrument < 0) return i;
    }
    // All full — steal oldest (slot 0)
    return 0;
}

int MidiInputRouter::findLiveChannel(int instrIdx, unsigned char note) {
    for (int i = 0; i < LIVE_CH_COUNT; i++) {
        if (liveNotes_[i].instrument == instrIdx &&
            liveNotes_[i].note == note) {
            return i;
        }
    }
    return -1;
}

void MidiInputRouter::freeLiveChannel(int slot) {
    if (slot >= 0 && slot < LIVE_CH_COUNT) {
        liveNotes_[slot].instrument = -1;
        liveNotes_[slot].note = 0;
    }
}

void MidiInputRouter::Update(Observable &o, I_ObservableData *d) {
    if (!d || !project_ || !mixer_) return;

    MidiMessage *msg = static_cast<MidiMessage *>(d);
    MidiMessage::Type type = msg->GetType();

    int midiChannel = msg->status_ & 0x0F;
    unsigned char data1 = msg->data1_ & 0x7F;
    unsigned char data2 = msg->data2_ & 0x7F;

    // Determine which MIDI input device index this came from
    MidiService *svc = MidiService::GetInstance();
    if (!svc) return;
    int deviceIdx = -1;
    {
        int idx = 0;
        IteratorPtr<MidiInDevice> it(svc->GetInIterator());
        for (it->Begin(); !it->IsDone(); it->Next(), idx++) {
            if (&it->CurrentItem() == &o) {
                deviceIdx = idx;
                break;
            }
        }
    }
    if (deviceIdx < 0) return;

    // Scan all instruments for matching IMDI/IMIC settings
    InstrumentBank *bank = project_->GetInstrumentBank();
    if (!bank) return;

    bool isNoteOn = (type == MidiMessage::MIDI_NOTE_ON && data2 > 0);
    bool isNoteOff = (type == MidiMessage::MIDI_NOTE_OFF ||
                      (type == MidiMessage::MIDI_NOTE_ON && data2 == 0));

    for (int i = 0; i < MAX_INSTRUMENT_COUNT; i++) {
        I_Instrument *instr = bank->GetInstrument(i);
        if (!instr || instr->IsEmpty()) continue;

        Variable *vDev = instr->FindVariable(IMDI);
        Variable *vCh = instr->FindVariable(IMIC);
        if (!vDev || !vCh) continue;

        int instrDev = vDev->GetInt();
        int instrCh = vCh->GetInt();

        if (instrDev != deviceIdx || instrCh != midiChannel) continue;

        // Match found — route to this instrument
        if (isNoteOn) {
            int slot = allocLiveChannel();
            int ch = LIVE_CH_BASE + slot;

            // Stop any note currently on this slot
            if (liveNotes_[slot].instrument >= 0) {
                mixer_->StopInstrument(ch);
            }

            liveNotes_[slot].instrument = i;
            liveNotes_[slot].note = data1;
            instr->SetNextVelocity(data2);
            mixer_->StartInstrument(ch, instr, data1, true);
        } else if (isNoteOff) {
            // Note-off: find the matching live channel and stop it
            int slot = findLiveChannel(i, data1);
            if (slot >= 0) {
                int ch = LIVE_CH_BASE + slot;
                mixer_->StopInstrument(ch);
                freeLiveChannel(slot);
            }
        } else if (type == MidiMessage::MIDI_CONTROLLER ||
                   type == MidiMessage::MIDI_AFTERTOUCH ||
                   type == MidiMessage::MIDI_CHANNEL_AFTERTOUCH ||
                   type == MidiMessage::MIDI_PITCH_BEND) {
            // Forward CC, aftertouch, pitch bend to the instrument
            instr->QueueMidiEvent(msg->status_, msg->data1_, msg->data2_);
        }
        break; // Only route to first matching instrument
    }
}

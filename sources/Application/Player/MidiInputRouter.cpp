#include "MidiInputRouter.h"
#include "PlayerMixer.h"
#include "Player.h"
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
    for (int i = 0; i < MAX_PLUGIN_SLOTS; i++) {
        pluginSlots_[i].instrument = -1;
        pluginSlots_[i].slot = -1;
        pluginSlots_[i].noteCount = 0;
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

// --- Per-note channel helpers (Sample/SoundFont) ---

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

// --- Plugin instrument helpers (VST3/LV2: single channel per instrument) ---

bool MidiInputRouter::isPluginInstrument(I_Instrument *instr) {
    InstrumentType t = instr->GetType();
    return (t == IT_VST3 || t == IT_LV2);
}

int MidiInputRouter::findPluginSlot(int instrIdx) {
    for (int i = 0; i < MAX_PLUGIN_SLOTS; i++) {
        if (pluginSlots_[i].instrument == instrIdx) return i;
    }
    return -1;
}

int MidiInputRouter::allocPluginSlot(int instrIdx) {
    // Allocate a plugin slot and a live channel for it
    int ps = -1;
    for (int i = 0; i < MAX_PLUGIN_SLOTS; i++) {
        if (pluginSlots_[i].instrument < 0) { ps = i; break; }
    }
    if (ps < 0) return -1; // no free plugin slots

    int ch = allocLiveChannel();
    // Mark the live channel as taken by this plugin
    liveNotes_[ch].instrument = instrIdx;
    liveNotes_[ch].note = 0xFF; // sentinel: plugin slot, not a single note

    pluginSlots_[ps].instrument = instrIdx;
    pluginSlots_[ps].slot = ch;
    pluginSlots_[ps].noteCount = 0;
    return ps;
}

void MidiInputRouter::freePluginSlot(int instrIdx) {
    int ps = findPluginSlot(instrIdx);
    if (ps < 0) return;
    freeLiveChannel(pluginSlots_[ps].slot);
    pluginSlots_[ps].instrument = -1;
    pluginSlots_[ps].slot = -1;
    pluginSlots_[ps].noteCount = 0;
}

// Clear any routing state that references `instrIdx` (plugin slots or
// live-note channels).  Used when an instrument is type-switched or
// removed to avoid stale routing state.
void MidiInputRouter::ClearInstrumentRouting(int instrIdx) {
    if (instrIdx < 0) return;

    // Free any plugin slot associated with this instrument
    int ps = findPluginSlot(instrIdx);
    if (ps >= 0) {
        freePluginSlot(instrIdx);
    }

    // Clear live note channels referencing this instrument
    for (int ln = 0; ln < LIVE_CH_COUNT; ++ln) {
        if (liveNotes_[ln].instrument == instrIdx) {
            freeLiveChannel(ln);
        }
    }

    /* Trace::Debug removed */
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

    /* Trace::Debug removed: incoming MIDI message */

    // Scan all instruments for matching IMDI/IMIC settings
    InstrumentBank *bank = project_->GetInstrumentBank();
    if (!bank) return;

    // Self-heal: clear stale plugin/live slots that reference an
    // instrument index which was replaced by a type-switch or deleted.
    for (int ps = 0; ps < MAX_PLUGIN_SLOTS; ++ps) {
        int instrIdx = pluginSlots_[ps].instrument;
        if (instrIdx < 0) continue;
        I_Instrument *inst = bank->GetInstrument(instrIdx);
        if (!inst || !isPluginInstrument(inst)) {
            /* Trace::Debug removed: clearing stale plugin slot */
            freePluginSlot(instrIdx);
        }
    }
    for (int ln = 0; ln < LIVE_CH_COUNT; ++ln) {
        int instrIdx = liveNotes_[ln].instrument;
        if (instrIdx < 0) continue;
        I_Instrument *inst = bank->GetInstrument(instrIdx);
        if (!inst || inst->IsEmpty()) {
            /* Trace::Debug removed: clearing stale live slot */
            freeLiveChannel(ln);
        }
    }

    bool isNoteOn = (type == MidiMessage::MIDI_NOTE_ON && data2 > 0);
    bool isNoteOff = (type == MidiMessage::MIDI_NOTE_OFF ||
                      (type == MidiMessage::MIDI_NOTE_ON && data2 == 0));

    // PRIORITY: if the currently selected instrument matches IMDI/IMIC,
    // route MIDI to it immediately (user expectation when re-assigning).
    Player *pl = Player::GetInstance();
    if (pl && pl->viewData_) {
        int sel = pl->viewData_->currentInstrument_;
        if (sel >= 0 && sel < MAX_INSTRUMENT_COUNT) {
            I_Instrument *selInstr = bank->GetInstrument(sel);
            if (selInstr && !selInstr->IsEmpty()) {
                Variable *svDev = selInstr->FindVariable(IMDI);
                Variable *svCh = selInstr->FindVariable(IMIC);
                if (svDev && svCh && svDev->GetInt() == deviceIdx && svCh->GetInt() == midiChannel) {
                    I_Instrument *instr = selInstr;
                    int i = sel;
                    /* Trace::Debug removed: selected-instrument matched */
                    // route exactly as for a matched instrument
                    if (isPluginInstrument(instr)) {
                        if (isNoteOn) {
                            int ps = findPluginSlot(i);
                            if (ps < 0) {
                                ps = allocPluginSlot(i);
                                if (ps >= 0) {
                                    int ch = LIVE_CH_BASE + pluginSlots_[ps].slot;
                                    instr->SetNextVelocity(data2);
                                    mixer_->StartInstrument(ch, instr, data1, true);
                                    pluginSlots_[ps].noteCount = 1;
                                }
                            } else {
                                instr->QueueMidiEvent(0x90, data1, data2);
                                pluginSlots_[ps].noteCount++;
                            }
                        } else if (isNoteOff) {
                            int ps = findPluginSlot(i);
                            if (ps >= 0) {
                                pluginSlots_[ps].noteCount--;
                                if (pluginSlots_[ps].noteCount <= 0) {
                                    instr->QueueMidiEvent(0x80, data1, 0);
                                    int ch = LIVE_CH_BASE + pluginSlots_[ps].slot;
                                    mixer_->StopInstrument(ch);
                                    freePluginSlot(i);
                                } else {
                                    instr->QueueMidiEvent(0x80, data1, 0);
                                }
                            }
                        } else if (type == MidiMessage::MIDI_CONTROLLER ||
                                   type == MidiMessage::MIDI_AFTERTOUCH ||
                                   type == MidiMessage::MIDI_CHANNEL_AFTERTOUCH ||
                                   type == MidiMessage::MIDI_PITCH_BEND) {
                            instr->QueueMidiEvent(msg->status_, msg->data1_, msg->data2_);
                        }
                    } else {
                        if (isNoteOn) {
                            int slot = allocLiveChannel();
                            int ch = LIVE_CH_BASE + slot;
                            if (liveNotes_[slot].instrument >= 0) {
                                mixer_->StopInstrument(ch);
                            }
                            liveNotes_[slot].instrument = i;
                            liveNotes_[slot].note = data1;
                            instr->SetNextVelocity(data2);
                            mixer_->StartInstrument(ch, instr, data1, true);
                        } else if (isNoteOff) {
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
                            instr->QueueMidiEvent(msg->status_, msg->data1_, msg->data2_);
                        }
                    }
                    return; // done routing to selected instrument
                }
            }
        }
    }

    for (int i = 0; i < MAX_INSTRUMENT_COUNT; i++) {
        I_Instrument *instr = bank->GetInstrument(i);
        if (!instr || instr->IsEmpty()) continue;

        Variable *vDev = instr->FindVariable(IMDI);
        Variable *vCh = instr->FindVariable(IMIC);
        if (!vDev || !vCh) continue;

        int instrDev = vDev->GetInt();
        int instrCh = vCh->GetInt();

        if (instrDev != deviceIdx || instrCh != midiChannel) continue;

        /* Trace::Debug removed: matched instrument routing */

        // ---- Plugin instruments: single channel, polyphonic MIDI ----
        if (isPluginInstrument(instr)) {
            if (isNoteOn) {
                int ps = findPluginSlot(i);
                if (ps < 0) {
                    // First note: allocate a channel and start the instrument
                    ps = allocPluginSlot(i);
                    if (ps < 0) break; // no slots available
                    int ch = LIVE_CH_BASE + pluginSlots_[ps].slot;
                    instr->SetNextVelocity(data2);
                    mixer_->StartInstrument(ch, instr, data1, true);
                    pluginSlots_[ps].noteCount = 1;
                } else {
                    // Additional notes: send via QueueMidiEvent (plugin handles polyphony)
                    instr->QueueMidiEvent(0x90, data1, data2);
                    pluginSlots_[ps].noteCount++;
                }
            } else if (isNoteOff) {
                int ps = findPluginSlot(i);
                if (ps >= 0) {
                    pluginSlots_[ps].noteCount--;
                    if (pluginSlots_[ps].noteCount <= 0) {
                        // Last note released: send note-off and stop channel
                        // (triggers release tail via PlayerChannel::releasing_)
                        instr->QueueMidiEvent(0x80, data1, 0);
                        int ch = LIVE_CH_BASE + pluginSlots_[ps].slot;
                        mixer_->StopInstrument(ch);
                        freePluginSlot(i);
                    } else {
                        // More notes still held: just send note-off to plugin
                        instr->QueueMidiEvent(0x80, data1, 0);
                    }
                }
            } else if (type == MidiMessage::MIDI_CONTROLLER ||
                       type == MidiMessage::MIDI_AFTERTOUCH ||
                       type == MidiMessage::MIDI_CHANNEL_AFTERTOUCH ||
                       type == MidiMessage::MIDI_PITCH_BEND) {
                instr->QueueMidiEvent(msg->status_, msg->data1_, msg->data2_);
            }
            break;
        }

        // ---- Non-plugin instruments: one channel per note ----
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
            instr->QueueMidiEvent(msg->status_, msg->data1_, msg->data2_);
        }
        break; // Only route to first matching instrument
    }
}

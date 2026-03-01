#include "MidiOutInstrument.h"
#include "CommandList.h"
#include "System/Console/Trace.h"
#include <string.h>

MidiService *MidiOutInstrument::svc_ = 0;

MidiOutInstrument::MidiOutInstrument() {
    strcpy(name_, "MidiOut");

    if (svc_ == 0) {
        svc_ = MidiService::GetInstance();
    }

    Variable *v = new Variable("device", MOIP_DEVICE, 0);
    Insert(v);

    v = new Variable("channel", MOIP_CHANNEL, 0);
    Insert(v);

    v = new Variable("note length", MOIP_NOTELENGTH, 0);
    Insert(v);

    v = new Variable("table", MOIP_TABLE, -1);
    Insert(v);

    v = new Variable("table automation", MOIP_TABLEAUTO, false);
    Insert(v);

    v = new Variable("transpose", MOIP_TRANSPOSE, 0);
    Insert(v);

    v = new Variable("send clock", MOIP_CLOCK, false);
    Insert(v);

    v = new Variable("send transport", MOIP_TRANSPORT, false);
    Insert(v);

    transportRunning_ = false;
    memset(lastNote_, 0, sizeof(lastNote_));
    memset(lastChannel_, 0, sizeof(lastChannel_));
    memset(lastDevice_, 0, sizeof(lastDevice_));
    memset(first_, 0, sizeof(first_));
    memset(playing_, 0, sizeof(playing_));
    memset(retrig_, 0, sizeof(retrig_));
    memset(retrigLoop_, 0, sizeof(retrigLoop_));
    memset(remainingTicks_, 0, sizeof(remainingTicks_));
    memset(velocity_, 127, sizeof(velocity_));
}

MidiOutInstrument::~MidiOutInstrument() {
}

bool MidiOutInstrument::Init() {
    tableState_.Reset();
    return true;
}

// Helper: send a MIDI message to the selected device
void MidiOutInstrument::sendMidi(MidiMessage &msg) {
    Variable *vd = FindVariable(MOIP_DEVICE);
    int dev = vd ? vd->GetInt() : 0;
    svc_->SendToDevice(msg, dev);
}

void MidiOutInstrument::sendMidiToDevice(MidiMessage &msg, int deviceIndex) {
    svc_->SendToDevice(msg, deviceIndex);
}

int MidiOutInstrument::GetDeviceCount() {
    return svc_ ? svc_->GetOutDeviceCount() : 0;
}

const char *MidiOutInstrument::GetDeviceName(int index) {
    return svc_ ? svc_->GetOutDeviceName(index) : "";
}

void MidiOutInstrument::OnStart() {
    tableState_.Reset();

    // Send note-offs for any channels still marked as playing from a
    // previous session BEFORE resetting state. This prevents stuck notes
    // on external synths when the user changes instruments between plays.
    for (int i = 0; i < SONG_CHANNEL_COUNT; i++) {
        if (playing_[i]) {
            MidiMessage off;
            off.status_ = MIDI_NOTE_OFF + lastChannel_[i];
            off.data1_ = lastNote_[i];
            off.data2_ = 0x00;
            sendMidiToDevice(off, lastDevice_[i]);
            playing_[i] = false;
        }
    }

    // Reset per-channel state
    memset(retrig_, 0, sizeof(retrig_));
    memset(first_, 0, sizeof(first_));

    // Send MIDI transport start (0xFA) if transport option is enabled
    Variable *v = FindVariable(MOIP_TRANSPORT);
    if (v && v->GetBool() && !transportRunning_) {
        MidiMessage msg;
        msg.status_ = 0xFA;
        msg.data1_ = MidiMessage::UNUSED_BYTE;
        msg.data2_ = MidiMessage::UNUSED_BYTE;
        sendMidi(msg);
        transportRunning_ = true;
    }
}

void MidiOutInstrument::OnStop() {
    // Send MIDI transport stop (0xFC) when the player stops, if transport was running
    if (transportRunning_) {
        Variable *vd = FindVariable(MOIP_DEVICE);
        int dev = vd ? vd->GetInt() : 0;
        MidiMessage stopMsg;
        stopMsg.status_ = 0xFC;
        stopMsg.data1_ = MidiMessage::UNUSED_BYTE;
        stopMsg.data2_ = MidiMessage::UNUSED_BYTE;
        sendMidiToDevice(stopMsg, dev);
        transportRunning_ = false;
    }
}

bool MidiOutInstrument::Start(int c, unsigned char note, bool retrigger) {
    if (c < 0 || c >= SONG_CHANNEL_COUNT) return false;

    Variable *vc = FindVariable(MOIP_CHANNEL);
    int mchannel = vc ? vc->GetInt() : 0;
    Variable *vd = FindVariable(MOIP_DEVICE);
    int dev = vd ? vd->GetInt() : 0;

    

    // Send note-off for previous note using the STORED channel/device
    // (the user may have changed channel/device since the note started)
    if (playing_[c]) {
        MidiMessage off;
        off.status_ = MIDI_NOTE_OFF + lastChannel_[c];
        off.data1_ = lastNote_[c];
        off.data2_ = 0x00;
        sendMidiToDevice(off, lastDevice_[c]);
    }

    first_[c] = true;

    // Store current channel and device for this note
    lastChannel_[c] = mchannel;
    lastDevice_[c] = dev;

    // Apply transpose
    Variable *v = FindVariable(MOIP_TRANSPOSE);
    int transpose = v ? v->GetInt() : 0;
    int transposed = (int)note + transpose;
    if (transposed < 0) transposed = 0;
    if (transposed > 127) transposed = 127;
    lastNote_[c] = transposed;

    v = FindVariable(MOIP_NOTELENGTH);
    remainingTicks_[c] = v ? v->GetInt() : 0;
    if (remainingTicks_[c] == 0) {
        remainingTicks_[c] = -1;
    }

    playing_[c] = true;
    retrig_[c] = false;

    return true;
}

void MidiOutInstrument::Stop(int c) {
    if (c < 0 || c >= SONG_CHANNEL_COUNT) return;
    if (!playing_[c]) {
        
        return;
    }

    // Use the stored channel/device from when the note was started
    int channel = lastChannel_[c];
    int dev = lastDevice_[c];

    

    // Send note-off for the last note to the correct device/channel
    MidiMessage msg;
    msg.status_ = MIDI_NOTE_OFF + channel;
    msg.data1_ = lastNote_[c];
    msg.data2_ = 0x00;
    sendMidiToDevice(msg, dev);

    playing_[c] = false;

    // Check if ANY channel is still playing this instrument
    bool anyPlaying = false;
    for (int i = 0; i < SONG_CHANNEL_COUNT; i++) {
        if (playing_[i]) { anyPlaying = true; break; }
    }

    // Send CC123 All Notes Off when the last note finishes
    if (!anyPlaying) {
        MidiMessage allOff;
        allOff.status_ = MIDI_CC + channel;
        allOff.data1_ = 123;
        allOff.data2_ = 0;
        sendMidiToDevice(allOff, dev);
    }

    // NOTE: transport stop (0xFC) is intentionally NOT sent here.
    // It is sent in OnStop(), which is called when the player globally stops.
}

bool MidiOutInstrument::Render(int channel, fixed *buffer, int size, bool updateTick) {
    if (channel < 0 || channel >= SONG_CHANNEL_COUNT) return false;

    // Use the stored channel/device from Start() for consistency
    int mchannel = lastChannel_[channel];
    int dev = lastDevice_[channel];

    if (first_[channel]) {
        
        MidiMessage msg;
        msg.status_ = MIDI_NOTE_ON + mchannel;
        msg.data1_ = lastNote_[channel];
        msg.data2_ = velocity_[channel];
        sendMidiToDevice(msg, dev);
        first_[channel] = false;
    }

    // Send MIDI clock (0xF8) if clock option is enabled
    // Only send from the lowest-numbered playing channel to avoid duplicates
    // when multiple channels share this instrument.
    Variable *v = FindVariable(MOIP_CLOCK);
    if (v && v->GetBool() && playing_[channel]) {
        SyncMaster *sm = SyncMaster::GetInstance();
        if (sm && sm->MidiSlice()) {
            bool isLowest = true;
            for (int i = 0; i < channel; i++) {
                if (playing_[i]) { isLowest = false; break; }
            }
            if (isLowest) {
                MidiMessage clockMsg;
                clockMsg.status_ = 0xF8;
                clockMsg.data1_ = MidiMessage::UNUSED_BYTE;
                clockMsg.data2_ = MidiMessage::UNUSED_BYTE;
                sendMidi(clockMsg);
            }
        }
    }

    // Only count down note length on table slices (matching SampleInstrument
    // tick rate).  Previously this decremented on every audio buffer, making
    // notes expire orders of magnitude too fast.
    if (updateTick && remainingTicks_[channel] > 0) {
        remainingTicks_[channel]--;
        if (remainingTicks_[channel] == 0) {
            if (!retrig_[channel]) {
                // Auto note-off: just send the specific note-off, don't use
                // Stop() which would send CC123 and transport stop
                
                MidiMessage offMsg;
                offMsg.status_ = MIDI_NOTE_OFF + mchannel;
                offMsg.data1_ = lastNote_[channel];
                offMsg.data2_ = 0x00;
                sendMidiToDevice(offMsg, dev);
                playing_[channel] = false;
            } else {
                MidiMessage msg;
                remainingTicks_[channel] = retrigLoop_[channel];
                msg.status_ = MIDI_NOTE_OFF + mchannel;
                msg.data1_ = lastNote_[channel];
                msg.data2_ = 0x00;
                sendMidiToDevice(msg, dev);
                msg.status_ = MIDI_NOTE_ON + mchannel;
                msg.data1_ = lastNote_[channel];
                msg.data2_ = velocity_[channel];
                sendMidiToDevice(msg, dev);
            }
        }
    }
    return false;
}

bool MidiOutInstrument::IsInitialized() {
    return true;
}

void MidiOutInstrument::ProcessCommand(int channel, FourCC cc, ushort value) {
    if (channel < 0 || channel >= SONG_CHANNEL_COUNT) return;

    // Use stored channel/device when a note is active so CCs/PB follow
    // the note even if the user changes the instrument's settings mid-note.
    // Fall back to live variables when no note is playing.
    int mchannel, dev;
    if (playing_[channel]) {
        mchannel = lastChannel_[channel];
        dev = lastDevice_[channel];
    } else {
        Variable *vc = FindVariable(MOIP_CHANNEL);
        mchannel = vc ? vc->GetInt() : 0;
        Variable *vd = FindVariable(MOIP_DEVICE);
        dev = vd ? vd->GetInt() : 0;
    }

    switch (cc) {
        case I_CMD_RTRG: {
            unsigned char loop = (value & 0xFF);
            if (loop != 0) {
                retrig_[channel] = true;
                retrigLoop_[channel] = loop;
                remainingTicks_[channel] = loop;
                
            } else {
                retrig_[channel] = false;
            }
        } break;

        case I_CMD_MVEL: {
            int vel = value / 2;
            if (vel > 127) vel = 127;
            velocity_[channel] = (unsigned char)vel;
        } break;

        case I_CMD_VOLM: {
            MidiMessage msg;
            msg.status_ = MIDI_CC + mchannel;
            msg.data1_ = 7;
            msg.data2_ = (unsigned char)(value / 2);
            sendMidiToDevice(msg, dev);
        } break;

        case I_CMD_MDCC: {
            MidiMessage msg;
            msg.status_ = MIDI_CC + mchannel;
            msg.data1_ = (value & 0x7F00) >> 8;
            msg.data2_ = (value & 0x7F);
            sendMidiToDevice(msg, dev);
        } break;

        case I_CMD_MDPG: {
            MidiMessage msg;
            msg.status_ = MIDI_PRG + mchannel;
            msg.data1_ = (value & 0x7F);
            msg.data2_ = MidiMessage::UNUSED_BYTE;
            sendMidiToDevice(msg, dev);
        } break;

        case I_CMD_LEGA: {
            MidiMessage msg;
            msg.status_ = MIDI_PB + mchannel;
            msg.data1_ = (unsigned char)(value & 0x7F);
            msg.data2_ = (unsigned char)((value >> 7) & 0x7F);
            sendMidiToDevice(msg, dev);
        } break;

        case I_CMD_PFIN: {
            int bend = 0x2000 + (int)(signed short)value;
            if (bend < 0) bend = 0;
            if (bend > 0x3FFF) bend = 0x3FFF;
            MidiMessage msg;
            msg.status_ = MIDI_PB + mchannel;
            msg.data1_ = (unsigned char)(bend & 0x7F);
            msg.data2_ = (unsigned char)((bend >> 7) & 0x7F);
            sendMidiToDevice(msg, dev);
        } break;

        case I_CMD_PAN_: {
            MidiMessage msg;
            msg.status_ = MIDI_CC + mchannel;
            msg.data1_ = 10;
            msg.data2_ = (unsigned char)(value & 0x7F);
            sendMidiToDevice(msg, dev);
        } break;

        case I_CMD_MCAT: {
            MidiMessage msg;
            msg.status_ = MIDI_CAT + mchannel;
            msg.data1_ = (unsigned char)(value & 0x7F);
            msg.data2_ = MidiMessage::UNUSED_BYTE;
            sendMidiToDevice(msg, dev);
        } break;

        case I_CMD_MPAT: {
            MidiMessage msg;
            msg.status_ = MIDI_PAT + mchannel;
            msg.data1_ = (unsigned char)((value >> 8) & 0x7F);
            msg.data2_ = (unsigned char)(value & 0x7F);
            sendMidiToDevice(msg, dev);
        } break;

        case I_CMD_MBNK: {
            // Send CC0 (Bank Select MSB) then CC32 (Bank Select LSB)
            MidiMessage msg;
            msg.status_ = MIDI_CC + mchannel;
            msg.data1_ = 0;  // CC0 = Bank Select MSB
            msg.data2_ = (unsigned char)((value >> 8) & 0x7F);
            sendMidiToDevice(msg, dev);
            MidiMessage msg2;
            msg2.status_ = MIDI_CC + mchannel;
            msg2.data1_ = 32; // CC32 = Bank Select LSB
            msg2.data2_ = (unsigned char)(value & 0x7F);
            sendMidiToDevice(msg2, dev);
        } break;
    }
}

const char *MidiOutInstrument::GetName() {
    Variable *vd = FindVariable(MOIP_DEVICE);
    Variable *vc = FindVariable(MOIP_CHANNEL);
    int dev = vd ? vd->GetInt() : 0;
    int ch = vc ? vc->GetInt() : 0;
    snprintf(name_, sizeof(name_), "MOUT D%d CH%02d", dev, ch + 1);
    return name_;
}

int MidiOutInstrument::GetTable() {
    Variable *v = FindVariable(MOIP_TABLE);
    return v ? v->GetInt() : -1;
}

bool MidiOutInstrument::GetTableAutomation() {
    Variable *v = FindVariable(MOIP_TABLEAUTO);
    return v ? v->GetBool() : false;
}

void MidiOutInstrument::GetTableState(TableSaveState &state) {
    memcpy(state.hopCount_, tableState_.hopCount_, sizeof(uchar) * TABLE_STEPS * 5);
    memcpy(state.position_, tableState_.position_, sizeof(int) * 5);
}

void MidiOutInstrument::SetTableState(TableSaveState &state) {
    memcpy(tableState_.hopCount_, state.hopCount_, sizeof(uchar) * TABLE_STEPS * 5);
    memcpy(tableState_.position_, state.position_, sizeof(int) * 5);
}

#include "AudioInInstrument.h"
#include "CommandList.h"
#include "System/Console/Trace.h"
#include "Services/Audio/Audio.h"
#include "Application/Mixer/MixerService.h"
#include "Foundation/T_Singleton.h"
#include <string.h>
#include <math.h>

MidiService *AudioInInstrument::svc_ = 0;

// ---------------------------------------------------------------------------
// CaptureModule – AudioModule that lives on a MixBus and renders every
// audio callback regardless of note triggers.
// ---------------------------------------------------------------------------

bool AudioInInstrument::CaptureModule::Render(fixed *buffer, int samplecount) {
    bool result = owner_.renderCapture(buffer, samplecount);
    // Apply effect chain (set via FXSN command) after capturing
    if (result && owner_.activeEffect_ && !owner_.activeEffect_->IsEmpty()) {
        owner_.activeEffect_->ProcessAudio(buffer, samplecount, owner_.effectWetDry_);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

AudioInInstrument::AudioInInstrument()
    : captureModule_(*this),
      currentBus_(nullptr)
{
    strcpy(name_, "AudioIn");

    if (svc_ == 0) {
        svc_ = MidiService::GetInstance();
    }

    // Audio input device selector
    Variable *v = new Variable("input device", AIP_INPUTDEVICE, 0);
    Insert(v);

    // Volume
    v = new Variable("volume", AIP_VOLUME, 255);
    Insert(v);

    // Pan
    v = new Variable("pan", AIP_PAN, 0x7F);
    Insert(v);

    // MIDI output device selector
    v = new Variable("midi device", AIP_MIDIDEVICE, 0);
    Insert(v);

    // MIDI channel (0-15)
    v = new Variable("midi channel", AIP_MIDICHANNEL, 0);
    Insert(v);

    // Note length
    v = new Variable("note length", AIP_NOTELENGTH, 0);
    Insert(v);

    // Transpose
    v = new Variable("transpose", AIP_TRANSPOSE, 0);
    Insert(v);

    // Table
    v = new Variable("table", AIP_TABLE, -1);
    Insert(v);

    // Table automation
    v = new Variable("table automation", AIP_TABLEAUTO, false);
    Insert(v);

    captureDeviceId_ = 0;
    currentInputDevice_ = -1;
    nativeRate_ = 44100;
    nativeChannels_ = 2;
    activeEffect_ = nullptr;
    effectWetDry_ = 255;
    memset(&captureSpec_, 0, sizeof(captureSpec_));
    memset(lastNote_, 0, sizeof(lastNote_));
    memset(lastMidiChannel_, 0, sizeof(lastMidiChannel_));
    memset(lastMidiDevice_, 0, sizeof(lastMidiDevice_));
    memset(first_, 0, sizeof(first_));
    memset(playing_, 0, sizeof(playing_));
    memset(remainingTicks_, 0, sizeof(remainingTicks_));
    memset(velocity_, 127, sizeof(velocity_));
}

AudioInInstrument::~AudioInInstrument() {
    unregisterFromBus();
    closeCaptureDevice();
}

// ---------------------------------------------------------------------------
// Bus registration – insert/remove CaptureModule on MixBus 0
// ---------------------------------------------------------------------------

void AudioInInstrument::registerOnBus() {
    if (currentBus_) return; // already registered
    MixerService *ms = MixerService::GetInstance();
    if (!ms) return;
    MixBus *bus = ms->GetMixBus(0);
    if (!bus) return;
    bus->Insert(captureModule_);
    currentBus_ = bus;
}

void AudioInInstrument::unregisterFromBus() {
    if (!currentBus_) return;
    currentBus_->Remove(captureModule_);
    currentBus_ = nullptr;
}

// ---------------------------------------------------------------------------
// Input device enumeration & capture device management
// ---------------------------------------------------------------------------

void AudioInInstrument::enumerateInputDevices() {
    inputDevices_.clear();
    int count = SDL_GetNumAudioDevices(1); // 1 = capture
    for (int i = 0; i < count; i++) {
        const char *name = SDL_GetAudioDeviceName(i, 1);
        if (name) {
            inputDevices_.push_back(std::string(name));
        }
    }
    if (inputDevices_.empty()) {
        inputDevices_.push_back("(no input found)");
    }
}

void AudioInInstrument::probeDeviceCapabilities() {
    nativeRate_ = 44100;
    nativeChannels_ = 1;

    Variable *vd = FindVariable(AIP_INPUTDEVICE);
    int devIdx = vd ? vd->GetInt() : 0;

    if (inputDevices_.empty()) {
        enumerateInputDevices();
    }
    if (devIdx < 0 || devIdx >= (int)inputDevices_.size()) {
        devIdx = 0;
    }
    if (inputDevices_.empty() || inputDevices_[devIdx] == "(no input found)") {
        return;
    }

    // Open briefly with ALLOW_ANY_CHANGE to discover native settings
    SDL_AudioSpec desired, obtained;
    SDL_zero(desired);
    desired.freq = 48000;
    desired.format = AUDIO_F32SYS;
    desired.channels = 2;
    desired.samples = 1024;
    desired.callback = NULL;

    const char *deviceName = inputDevices_[devIdx].c_str();
    SDL_AudioDeviceID probe = SDL_OpenAudioDevice(
        deviceName, 1, &desired, &obtained,
        SDL_AUDIO_ALLOW_ANY_CHANGE);

    if (probe != 0) {
        nativeRate_ = obtained.freq;
        nativeChannels_ = obtained.channels;
        SDL_CloseAudioDevice(probe);
    }
}

void AudioInInstrument::openCaptureDevice() {
    SysMutexLocker locker(captureMutex_);

    if (captureDeviceId_ != 0) {
        SDL_PauseAudioDevice(captureDeviceId_, 1);
        SDL_CloseAudioDevice(captureDeviceId_);
        captureDeviceId_ = 0;
        currentInputDevice_ = -1;
    }

    Variable *vd = FindVariable(AIP_INPUTDEVICE);
    int devIdx = vd ? vd->GetInt() : 0;

    if (inputDevices_.empty()) {
        enumerateInputDevices();
    }
    if (devIdx < 0 || devIdx >= (int)inputDevices_.size()) {
        devIdx = 0;
    }
    if (inputDevices_.empty() || inputDevices_[devIdx] == "(no input found)") {
        return;
    }

    // Request the OUTPUT sample rate so captured audio matches the
    // playback pipeline — no resampling needed in renderCapture().
    // Use native channel count from probe so SDL doesn't reject the open.
    // Don't allow frequency/channel changes: SDL converts internally.
    int targetRate = 44100;
    Audio *audio = Audio::GetInstance();
    if (audio) {
        targetRate = audio->GetSampleRate();
    }

    SDL_AudioSpec desired;
    SDL_zero(desired);
    desired.freq = targetRate;
    desired.format = AUDIO_F32SYS;
    desired.channels = nativeChannels_;
    desired.samples = 256;
    desired.callback = NULL; // queue-based capture

    const char *deviceName = inputDevices_[devIdx].c_str();
    SDL_AudioSpec obtained;
    SDL_AudioDeviceID id = SDL_OpenAudioDevice(
        deviceName, 1, &desired, &obtained, 0);

    if (id == 0) {
        Trace::Error("AudioIn: failed to open '%s': %s", deviceName, SDL_GetError());
    } else {
        captureSpec_ = obtained;
        currentInputDevice_ = devIdx;
        captureDeviceId_ = id;
        // Start capturing immediately
        SDL_PauseAudioDevice(captureDeviceId_, 0);
    }
}

void AudioInInstrument::closeCaptureDevice() {
    SysMutexLocker locker(captureMutex_);
    if (captureDeviceId_ != 0) {
        SDL_PauseAudioDevice(captureDeviceId_, 1);
        SDL_CloseAudioDevice(captureDeviceId_);
        captureDeviceId_ = 0;
        currentInputDevice_ = -1;
    }
}

// ---------------------------------------------------------------------------
// Init – lightweight; just enumerate devices. Does NOT open capture or
// register on the MixBus.  Call Activate() for that.
// ---------------------------------------------------------------------------

bool AudioInInstrument::Init() {
    tableState_.Reset();
    enumerateInputDevices();
    return true;
}

void AudioInInstrument::Activate() {
    if (currentBus_) return; // already active
    probeDeviceCapabilities();
    openCaptureDevice();
    registerOnBus();
}

// ---------------------------------------------------------------------------
// Always-on audio rendering (called from CaptureModule on the audio thread)
// ---------------------------------------------------------------------------

bool AudioInInstrument::renderCapture(fixed *buffer, int samplecount) {
    // If user changed the input device in the UI, re-open
    Variable *vid = FindVariable(AIP_INPUTDEVICE);
    int wantDev = vid ? vid->GetInt() : 0;
    if (currentInputDevice_ != wantDev && wantDev >= 0) {
        probeDeviceCapabilities();
        openCaptureDevice();
    }

    SysMutexLocker locker(captureMutex_);

    if (captureDeviceId_ == 0) {
        return false; // no device – produce silence
    }

    // Read volume / pan
    Variable *vv = FindVariable(AIP_VOLUME);
    int vol = vv ? vv->GetInt() : 255;
    Variable *vp = FindVariable(AIP_PAN);
    int pan = vp ? vp->GetInt() : 0x7F;

    // Normalised volume: 0..255 → fixed-point 0.0..1.0
    fixed volScale = fl2fp(1.0f / 255.0f);
    fixed fVol = fp_mul(i2fp(vol), volScale);

    // Pan: 0x00 = hard left, 0x7F = centre, 0xFE = hard right
    fixed volL, volR;
    if (pan <= 0x7F) {
        volR = fp_mul(fVol, fl2fp((float)pan / 127.0f));
        volL = fVol;
    } else {
        volL = fp_mul(fVol, fl2fp((float)(0xFE - pan) / 127.0f));
        volR = fVol;
    }

    // Dequeue captured F32 audio.
    // Just read what we need — one contiguous chunk per callback keeps the
    // waveform continuous (no clicks/static).  If clock drift causes the
    // queue to grow beyond a few callbacks' worth, flush it entirely and
    // accept one silent callback rather than constant splicing artifacts.
    int captureChannels = captureSpec_.channels;
    if (captureChannels < 1) captureChannels = 2;

    int floatsPerFrame = captureChannels;
    int wantFloats = samplecount * floatsPerFrame;
    if (wantFloats > 4096) wantFloats = 4096;
    int wantBytes = wantFloats * (int)sizeof(float);

    // If the queue has grown beyond ~4 render-chunks, clock drift has
    // accumulated.  Flush everything and start fresh.
    Uint32 queued = SDL_GetQueuedAudioSize(captureDeviceId_);
    if ((int)queued > wantBytes * 4) {
        SDL_ClearQueuedAudio(captureDeviceId_);
    }

    // Now dequeue exactly one contiguous chunk
    Uint32 bytesGot = SDL_DequeueAudio(captureDeviceId_, captureBuffer_, wantBytes);
    int framesGot = (int)(bytesGot / sizeof(float)) / floatsPerFrame;

    if (framesGot == 0 && vol == 0) {
        return false; // nothing to mix
    }

    // Convert F32 → fixed-point, apply volume/pan, write into buffer.
    // SDL capture delivers floats in -1.0..1.0.  The mixer pipeline works
    // in 16-bit-scale fixed-point (full-scale = i2fp(32767) ≈ 1073709056).
    // fl2fp(1.0) = 32768, which is only i2fp(1) — 32768× too quiet.
    // So scale the float by 32768 before fl2fp to match the pipeline.
    static const float F32_TO_S16_SCALE = 32768.0f;

    for (int i = 0; i < samplecount; i++) {
        fixed sampleL, sampleR;

        if (i < framesGot) {
            if (captureChannels >= 2) {
                sampleL = fl2fp(captureBuffer_[i * floatsPerFrame] * F32_TO_S16_SCALE);
                sampleR = fl2fp(captureBuffer_[i * floatsPerFrame + 1] * F32_TO_S16_SCALE);
            } else {
                fixed m = fl2fp(captureBuffer_[i * floatsPerFrame] * F32_TO_S16_SCALE);
                sampleL = m;
                sampleR = m;
            }
        } else {
            sampleL = 0;
            sampleR = 0;
        }

        buffer[i * 2]     = fp_mul(sampleL, volL);
        buffer[i * 2 + 1] = fp_mul(sampleR, volR);
    }

    return true;
}

// ---------------------------------------------------------------------------
// OnStart – song playback started; send MIDI note-offs for any held notes
// ---------------------------------------------------------------------------

void AudioInInstrument::OnStart() {
    tableState_.Reset();

    for (int i = 0; i < SONG_CHANNEL_COUNT; i++) {
        if (playing_[i]) {
            MidiMessage off;
            off.status_ = 0x80 + lastMidiChannel_[i];
            off.data1_ = lastNote_[i];
            off.data2_ = 0x00;
            sendMidiToDevice(off, lastMidiDevice_[i]);
            playing_[i] = false;
        }
    }
    memset(first_, 0, sizeof(first_));
}

// ---------------------------------------------------------------------------
// Start / Stop – MIDI only (audio capture is always-on)
// ---------------------------------------------------------------------------

bool AudioInInstrument::Start(int c, unsigned char note, bool retrigger) {
    if (c < 0 || c >= SONG_CHANNEL_COUNT) return false;

    Variable *vc = FindVariable(AIP_MIDICHANNEL);
    int mchannel = vc ? vc->GetInt() : 0;
    Variable *vd = FindVariable(AIP_MIDIDEVICE);
    int mdev = vd ? vd->GetInt() : 0;

    // Note-off for previous note
    if (playing_[c]) {
        MidiMessage off;
        off.status_ = 0x80 + lastMidiChannel_[c];
        off.data1_ = lastNote_[c];
        off.data2_ = 0x00;
        sendMidiToDevice(off, lastMidiDevice_[c]);
    }

    first_[c] = true;
    lastMidiChannel_[c] = mchannel;
    lastMidiDevice_[c] = mdev;

    // Transpose
    Variable *v = FindVariable(AIP_TRANSPOSE);
    int transpose = v ? v->GetInt() : 0;
    int transposed = (int)note + transpose;
    if (transposed < 0) transposed = 0;
    if (transposed > 127) transposed = 127;
    lastNote_[c] = transposed;

    v = FindVariable(AIP_NOTELENGTH);
    remainingTicks_[c] = v ? v->GetInt() : 0;
    if (remainingTicks_[c] == 0) {
        remainingTicks_[c] = -1; // infinite
    }

    playing_[c] = true;
    return true;
}

void AudioInInstrument::Stop(int c) {
    if (c < 0 || c >= SONG_CHANNEL_COUNT) return;
    if (!playing_[c]) return;

    MidiMessage msg;
    msg.status_ = 0x80 + lastMidiChannel_[c];
    msg.data1_ = lastNote_[c];
    msg.data2_ = 0x00;
    sendMidiToDevice(msg, lastMidiDevice_[c]);
    playing_[c] = false;
}

// ---------------------------------------------------------------------------
// I_Instrument::Render – MIDI timing only, no audio production
// ---------------------------------------------------------------------------

bool AudioInInstrument::Render(int channel, fixed *buffer, int size, bool updateTick) {
    if (channel < 0 || channel >= SONG_CHANNEL_COUNT) return false;

    // Send deferred MIDI note-on
    if (first_[channel]) {
        int mchannel = lastMidiChannel_[channel];
        int mdev = lastMidiDevice_[channel];
        MidiMessage msg;
        msg.status_ = 0x90 + mchannel;
        msg.data1_ = lastNote_[channel];
        msg.data2_ = velocity_[channel];
        sendMidiToDevice(msg, mdev);
        first_[channel] = false;
    }

    // Note length countdown
    if (updateTick && remainingTicks_[channel] > 0) {
        remainingTicks_[channel]--;
        if (remainingTicks_[channel] == 0) {
            MidiMessage offMsg;
            offMsg.status_ = 0x80 + lastMidiChannel_[channel];
            offMsg.data1_ = lastNote_[channel];
            offMsg.data2_ = 0x00;
            sendMidiToDevice(offMsg, lastMidiDevice_[channel]);
        }
    }

    // No audio – live audio comes through CaptureModule on the MixBus
    return false;
}

bool AudioInInstrument::IsInitialized() {
    return true;
}

// ---------------------------------------------------------------------------
// ProcessCommand – MIDI CC / program change / pitch bend etc.
// ---------------------------------------------------------------------------

void AudioInInstrument::ProcessCommand(int channel, FourCC cc, ushort value) {
    if (channel < 0 || channel >= SONG_CHANNEL_COUNT) return;

    int mchannel, mdev;
    if (playing_[channel]) {
        mchannel = lastMidiChannel_[channel];
        mdev = lastMidiDevice_[channel];
    } else {
        Variable *vc = FindVariable(AIP_MIDICHANNEL);
        mchannel = vc ? vc->GetInt() : 0;
        Variable *vd = FindVariable(AIP_MIDIDEVICE);
        mdev = vd ? vd->GetInt() : 0;
    }

    switch (cc) {
        case I_CMD_VOLM: {
            Variable *vv = FindVariable(AIP_VOLUME);
            if (vv) vv->SetInt(value & 0xFF);
        } break;

        case I_CMD_PAN_: {
            Variable *vp = FindVariable(AIP_PAN);
            if (vp) vp->SetInt(value & 0xFE);
        } break;

        case I_CMD_MVEL: {
            int vel = value / 2;
            if (vel > 127) vel = 127;
            velocity_[channel] = (unsigned char)vel;
        } break;

        case I_CMD_MDCC: {
            MidiMessage msg;
            msg.status_ = 0xB0 + mchannel;
            msg.data1_ = (value & 0x7F00) >> 8;
            msg.data2_ = (value & 0x7F);
            sendMidiToDevice(msg, mdev);
        } break;

        case I_CMD_MDPG: {
            MidiMessage msg;
            msg.status_ = 0xC0 + mchannel;
            msg.data1_ = (value & 0x7F);
            msg.data2_ = MidiMessage::UNUSED_BYTE;
            sendMidiToDevice(msg, mdev);
        } break;

        case I_CMD_LEGA: {
            MidiMessage msg;
            msg.status_ = 0xE0 + mchannel;
            msg.data1_ = (unsigned char)(value & 0x7F);
            msg.data2_ = (unsigned char)((value >> 7) & 0x7F);
            sendMidiToDevice(msg, mdev);
        } break;

        case I_CMD_PFIN: {
            int bend = 0x2000 + (int)(signed short)value;
            if (bend < 0) bend = 0;
            if (bend > 0x3FFF) bend = 0x3FFF;
            MidiMessage msg;
            msg.status_ = 0xE0 + mchannel;
            msg.data1_ = (unsigned char)(bend & 0x7F);
            msg.data2_ = (unsigned char)((bend >> 7) & 0x7F);
            sendMidiToDevice(msg, mdev);
        } break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// GetName / Table / Device accessors
// ---------------------------------------------------------------------------

const char *AudioInInstrument::GetName() {
    Variable *vd = FindVariable(AIP_INPUTDEVICE);
    Variable *vc = FindVariable(AIP_MIDICHANNEL);
    int dev = vd ? vd->GetInt() : 0;
    int ch = vc ? vc->GetInt() : 0;
    snprintf(name_, sizeof(name_), "AIN D%d M%02d", dev, ch + 1);
    return name_;
}

int AudioInInstrument::GetTable() {
    Variable *v = FindVariable(AIP_TABLE);
    return v ? v->GetInt() : -1;
}

bool AudioInInstrument::GetTableAutomation() {
    Variable *v = FindVariable(AIP_TABLEAUTO);
    return v ? v->GetBool() : false;
}

void AudioInInstrument::GetTableState(TableSaveState &state) {
    memcpy(state.hopCount_, tableState_.hopCount_, sizeof(uchar) * TABLE_STEPS * 3);
    memcpy(state.position_, tableState_.position_, sizeof(int) * 3);
}

void AudioInInstrument::SetTableState(TableSaveState &state) {
    memcpy(tableState_.hopCount_, state.hopCount_, sizeof(uchar) * TABLE_STEPS * 3);
    memcpy(tableState_.position_, state.position_, sizeof(int) * 3);
}

int AudioInInstrument::GetInputDeviceCount() {
    if (inputDevices_.empty()) {
        enumerateInputDevices();
    }
    return (int)inputDevices_.size();
}

const char *AudioInInstrument::GetInputDeviceName(int index) {
    if (inputDevices_.empty()) {
        enumerateInputDevices();
    }
    if (index >= 0 && index < (int)inputDevices_.size()) {
        return inputDevices_[index].c_str();
    }
    return "";
}

int AudioInInstrument::GetMidiDeviceCount() {
    return svc_ ? svc_->GetOutDeviceCount() : 0;
}

const char *AudioInInstrument::GetMidiDeviceName(int index) {
    return svc_ ? svc_->GetOutDeviceName(index) : "";
}

void AudioInInstrument::SetEffect(I_Effect *effect, int wetDry) {
    activeEffect_ = effect;
    effectWetDry_ = wetDry;
}

void AudioInInstrument::ClearEffect() {
    activeEffect_ = nullptr;
    effectWetDry_ = 255;
}

void AudioInInstrument::sendMidi(MidiMessage &msg) {
    Variable *vd = FindVariable(AIP_MIDIDEVICE);
    int dev = vd ? vd->GetInt() : 0;
    if (svc_) svc_->SendToDevice(msg, dev);
}

void AudioInInstrument::sendMidiToDevice(MidiMessage &msg, int deviceIndex) {
    if (svc_) svc_->SendToDevice(msg, deviceIndex);
}

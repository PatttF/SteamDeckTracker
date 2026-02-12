#include "RecordSampleDialog.h"
#include "Application/Instruments/SamplePool.h"
#include "Application/AppWindow.h"
#include "Adapters/SDL2/GUI/SDLGUIWindowImp.h"
#include "System/Console/Trace.h"
#include "UIFramework/BasicDatas/GUIEvent.h"
#include <cstring>
#include <cmath>

const int RecordSampleDialog::sampleRates_[] = { 22050, 44100, 48000 };
const int RecordSampleDialog::numSampleRates_ = 3;

RecordSampleDialog::RecordSampleDialog(View &view)
    : ModalView(view),
      phase_(RP_SETUP),
      selected_(0),
      currentDevice_(0),
      sampleRateIdx_(1),     // default 44100
      channelIdx_(0),        // default mono
      captureDeviceId_(0),
      recordedSamples_(0),
      peakLevel_(0.0f),
      isRecording_(false),
      peakHold_(0.0f),
      peakHoldTimer_(0),
      recordTimerID_(0),
      previewDeviceId_(0),
      previewPos_(0),
      isPreviewing_(false),
      currentChar_(0),
      keyboardMode_(false),
      keyboardRow_(2),
      keyboardCol_(0),
      lastWindowWidth_(0),
      lastWindowHeight_(0),
      nativeRate_(44100),
      nativeChannels_(1)
{
    memset(name_, ' ', REC_MAX_NAME + 1);
    name_[REC_MAX_NAME] = 0;
}

RecordSampleDialog::~RecordSampleDialog() {
    stopPreview();
    if (recordTimerID_ != 0) {
        SDL_RemoveTimer(recordTimerID_);
        recordTimerID_ = 0;
    }
    closeCaptureDevice();
}

// ---- Audio device management ----

void RecordSampleDialog::enumerateInputDevices() {
    inputDevices_.clear();
    int count = SDL_GetNumAudioDevices(1);
    for (int i = 0; i < count; i++) {
        const char *name = SDL_GetAudioDeviceName(i, 1);
        if (name) {
            inputDevices_.push_back(std::string(name));
        }
    }
    if (inputDevices_.empty()) {
        inputDevices_.push_back("(no input found)");
    }
    if (currentDevice_ >= (int)inputDevices_.size()) {
        currentDevice_ = 0;
    }
    // Probe the current device's native capabilities
    probeDeviceCapabilities();
}

void RecordSampleDialog::probeDeviceCapabilities() {
    nativeRate_ = 44100;
    nativeChannels_ = 1;

    if (inputDevices_.empty() ||
        inputDevices_[currentDevice_] == "(no input found)") {
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

    const char *deviceName = inputDevices_[currentDevice_].c_str();
    SDL_AudioDeviceID probe = SDL_OpenAudioDevice(
        deviceName, 1, &desired, &obtained,
        SDL_AUDIO_ALLOW_ANY_CHANGE);

    if (probe != 0) {
        nativeRate_ = obtained.freq;
        nativeChannels_ = obtained.channels;
        Trace::Log("RECORD", "Probed %s: %d Hz, %d ch, fmt=0x%04X",
                   deviceName, obtained.freq, obtained.channels, obtained.format);
        SDL_CloseAudioDevice(probe);
    } else {
        Trace::Log("RECORD", "Probe failed for %s: %s", deviceName, SDL_GetError());
    }
}

void RecordSampleDialog::openCaptureDevice() {
    closeCaptureDevice();

    // Always request F32 at the device's native rate/channels.
    // F32 is the native format on modern Linux audio stacks
    // (PipeWire, PulseAudio) so no SDL conversion is needed.
    SDL_AudioSpec desired;
    SDL_zero(desired);
    desired.freq = nativeRate_;
    desired.format = AUDIO_F32SYS;
    desired.channels = nativeChannels_;
    desired.samples = 1024;
    desired.callback = NULL;

    const char *deviceName = NULL;
    if (currentDevice_ < (int)inputDevices_.size() &&
        inputDevices_[currentDevice_] != "(no input found)") {
        deviceName = inputDevices_[currentDevice_].c_str();
    }

    captureDeviceId_ = SDL_OpenAudioDevice(
        deviceName, 1, &desired, &captureSpec_,
        SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);

    if (captureDeviceId_ == 0) {
        Trace::Error("RecordSample: failed to open capture device: %s",
                     SDL_GetError());
    } else {
        Trace::Log("RECORD", "Opened capture: %s at %d Hz, %d ch, fmt=0x%04X",
                   deviceName ? deviceName : "default",
                   captureSpec_.freq, captureSpec_.channels,
                   captureSpec_.format);
    }
}

void RecordSampleDialog::closeCaptureDevice() {
    if (captureDeviceId_ != 0) {
        SDL_CloseAudioDevice(captureDeviceId_);
        captureDeviceId_ = 0;
    }
}

// ---- Timer callback to wake SDL_WaitEvent during recording ----

Uint32 RecordSampleDialog::recordTimerCallback(Uint32 interval, void *param) {
    (void)param;
    // Push a user event to wake SDL_WaitEvent and trigger a redraw
    // through onEvent -> _isDirty -> Redraw -> DrawView
    GUIEvent *ev = new GUIEvent(0, ET_PLAYERUPDATE, SDL_GetTicks());
    SDL_Event event;
    SDL_memset(&event, 0, sizeof(event));
    event.type = SDL_USEREVENT;
    event.user.data1 = ev;
    SDL_PushEvent(&event);
    return interval; // keep firing
}

// ---- Poll captured audio and update meter ----

void RecordSampleDialog::pollAudio() {
    if (!isRecording_ || captureDeviceId_ == 0) return;

    int channels = captureSpec_.channels;
    // Buffer for F32 samples — we always open in F32 mode
    float fBuf[4096];
    Uint32 bytesAvail;
    float framePeak = 0.0f;

    while ((bytesAvail = SDL_DequeueAudio(captureDeviceId_, fBuf,
                                           sizeof(fBuf))) > 0) {
        int samplesRead = bytesAvail / sizeof(float);
        int framesRead = samplesRead / channels;
        int maxFrames = captureSpec_.freq * REC_MAX_SECONDS;
        int remaining = maxFrames - recordedSamples_;
        if (remaining <= 0) {
            stopRecording();
            return;
        }
        if (framesRead > remaining) framesRead = remaining;

        // Convert F32 to S16 and compute peak
        int totalSamples = framesRead * channels;
        for (int i = 0; i < totalSamples; i++) {
            float f = fBuf[i];
            float af = fabsf(f);
            if (af > framePeak) framePeak = af;

            // Clamp and convert to S16
            if (f > 1.0f) f = 1.0f;
            if (f < -1.0f) f = -1.0f;
            recordBuffer_.push_back((short)(f * 32767.0f));
        }
        recordedSamples_ += framesRead;
    }

    // Fast attack, smooth decay — always decay even if no new audio
    // arrived this tick (gotAudio == false). framePeak stays 0 in that
    // case, so we just take the decay branch.
    if (framePeak > peakLevel_) {
        peakLevel_ = framePeak;
    } else {
        peakLevel_ *= 0.80f;  // fast decay per tick
        if (peakLevel_ < 0.001f) peakLevel_ = 0.0f;
    }

    // Peak hold — stays at max then decays slowly
    if (framePeak > peakHold_) {
        peakHold_ = framePeak;
        peakHoldTimer_ = 0;
    } else {
        peakHoldTimer_++;
        if (peakHoldTimer_ > 30) {
            peakHold_ *= 0.97f;
            if (peakHold_ < 0.001f) peakHold_ = 0.0f;
        }
    }
}

// ---- Recording control ----

void RecordSampleDialog::startRecording() {
    if (inputDevices_.empty() ||
        inputDevices_[currentDevice_] == "(no input found)") {
        return;
    }

    openCaptureDevice();
    if (captureDeviceId_ == 0) return;

    int maxFrames = captureSpec_.freq * REC_MAX_SECONDS;
    int channels = captureSpec_.channels;
    recordBuffer_.clear();
    recordBuffer_.reserve(maxFrames * channels);
    recordedSamples_ = 0;
    peakLevel_ = 0.0f;
    peakHold_ = 0.0f;
    peakHoldTimer_ = 0;
    isRecording_ = true;

    SDL_PauseAudioDevice(captureDeviceId_, 0);

    // Start a timer to periodically wake the event loop for meter updates
    recordTimerID_ = SDL_AddTimer(50, recordTimerCallback, this);

    phase_ = RP_RECORDING;
    lastWindowWidth_ = 0;
    lastWindowHeight_ = 0;
    selected_ = 0;
}

void RecordSampleDialog::stopRecording() {
    if (!isRecording_) return;

    // Stop the periodic redraw timer
    if (recordTimerID_ != 0) {
        SDL_RemoveTimer(recordTimerID_);
        recordTimerID_ = 0;
    }

    if (captureDeviceId_ != 0) {
        SDL_PauseAudioDevice(captureDeviceId_, 1);
    }

    // Drain remaining audio (F32 format — same as pollAudio)
    if (captureDeviceId_ != 0) {
        int channels = captureSpec_.channels;
        float fBuf[4096];
        Uint32 bytesAvail;
        while ((bytesAvail = SDL_DequeueAudio(captureDeviceId_, fBuf,
                                               sizeof(fBuf))) > 0) {
            int samplesRead = bytesAvail / sizeof(float);
            int framesRead = samplesRead / channels;
            int maxFrames = captureSpec_.freq * REC_MAX_SECONDS;
            int remaining = maxFrames - recordedSamples_;
            if (remaining <= 0) break;
            if (framesRead > remaining) framesRead = remaining;
            int totalSamples = framesRead * channels;
            for (int i = 0; i < totalSamples; i++) {
                float f = fBuf[i];
                if (f > 1.0f) f = 1.0f;
                if (f < -1.0f) f = -1.0f;
                recordBuffer_.push_back((short)(f * 32767.0f));
            }
            recordedSamples_ += framesRead;
        }
    }

    isRecording_ = false;
    closeCaptureDevice();

    // Force full screen repaint to clear pixel-level meter artifacts
    ((AppWindow &)w_).InvalidateScreenCache();

    if (recordedSamples_ > 0) {
        phase_ = RP_SAVE;
        lastWindowWidth_ = 0;
        lastWindowHeight_ = 0;
        selected_ = 0;
        currentChar_ = 0;
        memset(name_, ' ', REC_MAX_NAME);
        name_[REC_MAX_NAME] = 0;
        keyboardMode_ = false;
    } else {
        phase_ = RP_SETUP;
        lastWindowWidth_ = 0;
        lastWindowHeight_ = 0;
        selected_ = 0;
    }
}

// ---- Preview Playback ----

void previewAudioCallback(void *userdata, Uint8 *stream, int len) {
    RecordSampleDialog *dlg = (RecordSampleDialog *)userdata;
    // This is called from the audio thread — we access preview state directly
    // since the main thread only sets isPreviewing_ = false or calls stopPreview
    short *out = (short *)stream;
    int samplesWanted = len / sizeof(short);
    int channels = dlg->captureSpec_.channels;
    int totalSamples = dlg->recordedSamples_ * channels;
    int pos = dlg->previewPos_ * channels;

    int written = 0;
    while (written < samplesWanted && pos < totalSamples) {
        out[written] = dlg->recordBuffer_[pos];
        written++;
        pos++;
    }
    // Fill remainder with silence
    while (written < samplesWanted) {
        out[written] = 0;
        written++;
    }

    dlg->previewPos_ = pos / channels;
    if (dlg->previewPos_ >= dlg->recordedSamples_) {
        dlg->isPreviewing_ = false;
    }
}

void RecordSampleDialog::startPreview() {
    stopPreview();

    SDL_AudioSpec desired;
    SDL_zero(desired);
    desired.freq = captureSpec_.freq;
    desired.format = AUDIO_S16SYS;
    desired.channels = captureSpec_.channels;
    desired.samples = 1024;
    desired.callback = previewAudioCallback;
    desired.userdata = this;

    previewDeviceId_ = SDL_OpenAudioDevice(NULL, 0, &desired, NULL, 0);
    if (previewDeviceId_ == 0) {
        Trace::Error("RecordSample: failed to open preview device: %s",
                     SDL_GetError());
        return;
    }

    previewPos_ = 0;
    isPreviewing_ = true;

    // Start timer for UI updates during preview
    if (recordTimerID_ == 0) {
        recordTimerID_ = SDL_AddTimer(50, recordTimerCallback, this);
    }

    SDL_PauseAudioDevice(previewDeviceId_, 0);
}

void RecordSampleDialog::stopPreview() {
    if (previewDeviceId_ != 0) {
        SDL_PauseAudioDevice(previewDeviceId_, 1);
        SDL_CloseAudioDevice(previewDeviceId_);
        previewDeviceId_ = 0;
    }
    isPreviewing_ = false;

    // Stop timer if not recording
    if (!isRecording_ && recordTimerID_ != 0) {
        SDL_RemoveTimer(recordTimerID_);
        recordTimerID_ = 0;
    }
}

// ---- Folder / Save ----

void RecordSampleDialog::saveRecording() {
    int end = REC_MAX_NAME - 1;
    while (end >= 0 && name_[end] == ' ') {
        name_[end] = 0;
        end--;
    }
    if (end < 0) return;

    // Build path: samplelib/recordings/<name>.wav
    const char *slpath = SamplePool::GetInstance()->GetSampleLib();
    Path slp(slpath);
    std::string recDir = slp.GetPath();
    if (recDir.back() != '/') recDir += '/';
    recDir += "recordings";

    // Create the recordings directory if it doesn't exist
    FileSystem::GetInstance()->MakeDir(recDir.c_str());

    std::string filename(name_);
    filename += ".wav";
    std::string fullPath = recDir + "/" + filename;

    I_File *file = FileSystem::GetInstance()->Open(fullPath.c_str(), (char *)"wb");
    if (!file) {
        Trace::Error("RecordSample: failed to create %s", fullPath.c_str());
        return;
    }

    int channels = captureSpec_.channels;
    int rate = captureSpec_.freq;
    int totalSamples = recordedSamples_ * channels;
    int dataSize = totalSamples * 2;

    unsigned int chunk, size;
    unsigned short us;

    chunk = 0x46464952; file->Write(&chunk, 1, 4);
    size = 36 + dataSize; file->Write(&size, 1, 4);
    chunk = 0x45564157; file->Write(&chunk, 1, 4);
    chunk = 0x20746D66; file->Write(&chunk, 1, 4);
    size = 16; file->Write(&size, 1, 4);
    us = 1; file->Write(&us, 1, 2);
    us = (unsigned short)channels; file->Write(&us, 1, 2);
    unsigned int sr = (unsigned int)rate; file->Write(&sr, 1, 4);
    unsigned int br = (unsigned int)(rate * channels * 2); file->Write(&br, 1, 4);
    us = (unsigned short)(channels * 2); file->Write(&us, 1, 2);
    us = 16; file->Write(&us, 1, 2);
    chunk = 0x61746164; file->Write(&chunk, 1, 4);
    unsigned int ds = (unsigned int)dataSize; file->Write(&ds, 1, 4);

    if (totalSamples > 0) {
        file->Write(recordBuffer_.data(), 2, totalSamples);
    }

    file->Close();
    delete file;

    savedPath_ = fullPath;
    Trace::Log("RECORD", "Saved recording to %s (%d frames, %d ch, %d Hz)",
               fullPath.c_str(), recordedSamples_, channels, rate);
}

void RecordSampleDialog::moveCursor(int direction) {
    int newPos = currentChar_ + direction;
    if (newPos >= 0 && newPos < REC_MAX_NAME) {
        currentChar_ = newPos;
        findCharacterInKeyboard(name_[currentChar_], keyboardRow_, keyboardCol_);
    }
}

// ---- Drawing ----

void RecordSampleDialog::DrawView() {
    // Poll captured audio on every redraw during recording
    if (phase_ == RP_RECORDING) {
        pollAudio();
    }

    // Check if preview finished playing
    if (phase_ == RP_SAVE && previewDeviceId_ != 0 && !isPreviewing_) {
        stopPreview();
    }
    switch (phase_) {
        case RP_SETUP:     drawSetupPhase(); break;
        case RP_RECORDING: drawRecordingPhase(); break;
        case RP_SAVE:      drawSavePhase(); break;
    }
}

void RecordSampleDialog::drawSetupPhase() {
    int w = 30, h = 9;
    if (w != lastWindowWidth_ || h != lastWindowHeight_) {
        SetWindow(w, h);
        lastWindowWidth_ = w;
        lastWindowHeight_ = h;
    }

    GUITextProperties props;
    SetColor(CD_NORMAL);

    DrawString(1, 0, "Record Sample", props);

    // Input device
    SetColor(CD_NORMAL);
    DrawString(1, 2, "Input:", props);
    props.invert_ = (selected_ == 0);
    if (selected_ == 0) SetColor(CD_HILITE2);
    char devBuf[32];
    if (!inputDevices_.empty()) {
        std::string dn = inputDevices_[currentDevice_];
        if ((int)dn.size() > 21) dn = dn.substr(0, 21);
        snprintf(devBuf, sizeof(devBuf), "%-21s", dn.c_str());
    } else {
        snprintf(devBuf, sizeof(devBuf), "%-21s", "(none)");
    }
    DrawString(8, 2, devBuf, props);
    props.invert_ = false;

    // Show detected device capabilities (read-only)
    SetColor(CD_NORMAL);
    char infoBuf[40];
    snprintf(infoBuf, sizeof(infoBuf), "Rate: %-20d", nativeRate_);
    DrawString(1, 4, infoBuf, props);
    snprintf(infoBuf, sizeof(infoBuf), "Chan: %-20s",
             nativeChannels_ > 1 ? "Stereo" : "Mono");
    DrawString(1, 5, infoBuf, props);

    // Buttons: Record and Cancel on same row
    int y = 7;
    SetColor(CD_NORMAL);
    props.invert_ = (selected_ == 1);
    if (selected_ == 1) SetColor(CD_HILITE2);
    DrawString(5, y, "Record", props);
    props.invert_ = false;
    SetColor(CD_NORMAL);
    props.invert_ = (selected_ == 2);
    if (selected_ == 2) SetColor(CD_HILITE2);
    DrawString(19, y, "Cancel", props);
    props.invert_ = false;
}

// Pixel-level meter drawing — called after text buffer flush
void RecordSampleDialog::DrawGraphics() {
    if (phase_ != RP_RECORDING) return;

    SDLGUIWindowImp *imp = (SDLGUIWindowImp *)w_.GetImpWindow();
    if (!imp) return;

    SDL_Renderer *renderer = imp->GetRenderer();
    if (!renderer) return;
    int mult = imp->GetMult();
    int ax = imp->GetAppAnchorX();
    int ay = imp->GetAppAnchorY();

    // Meter area: char columns 1..(width-1), rows 3-4
    // Convert to pixel coordinates using modal's left_/top_ offset
    int charWidth = 8;   // pixels per char cell
    int charHeight = 8;
    int meterCharW = 28; // width - 2 (width=30, 1 col margin each side)
    int px = (left_ + 1) * charWidth;
    int py = (top_ + 3) * charHeight + 1;  // 1px inset top
    int pw = meterCharW * charWidth;        // pixel width
    int ph = 2 * charHeight - 2;            // 1px inset top+bottom

    // Dark background
    SDL_SetRenderDrawColor(renderer, 0x18, 0x18, 0x18, 0xFF);
    SDL_Rect bgRect = { px * mult + ax, py * mult + ay, pw * mult, ph * mult };
    SDL_RenderFillRect(renderer, &bgRect);

    // Filled level bar with smooth gradient (left=green, right=red)
    float meterFrac = 0.0f;
    if (peakLevel_ > 0.0001f) {
        float db = 20.0f * log10f(peakLevel_);
        const float dbMin = -48.0f;
        const float dbMax = 6.0f;
        meterFrac = (db - dbMin) / (dbMax - dbMin);
        if (meterFrac < 0.0f) meterFrac = 0.0f;
        if (meterFrac > 1.0f) meterFrac = 1.0f;
    }
    int filledPx = (int)(meterFrac * pw);
    if (filledPx > pw) filledPx = pw;
    if (filledPx > 0) {
        // Draw in 2px-wide vertical strips with interpolated color
        for (int x = 0; x < filledPx; x += 2) {
            float t = (float)x / (float)pw; // 0.0 at left, 1.0 at right

            int r, g, b;
            if (t < 0.65f) {
                float s = t / 0.65f;
                r = (int)(0x22 + s * (0xCC - 0x22));
                g = 0xCC;
                b = (int)(0x44 - s * 0x44);
            } else if (t < 0.85f) {
                float s = (t - 0.65f) / 0.20f;
                r = (int)(0xCC + s * (0xEE - 0xCC));
                g = (int)(0xCC - s * (0xCC - 0x77));
                b = 0x00;
            } else {
                float s = (t - 0.85f) / 0.15f;
                r = (int)(0xEE + s * (0xFF - 0xEE));
                g = (int)(0x77 - s * 0x77);
                b = 0x00;
            }

            SDL_SetRenderDrawColor(renderer, r, g, b, 0xFF);
            int w2 = (x + 2 <= filledPx) ? 2 : filledPx - x;
            SDL_Rect sr = { (px + x) * mult + ax, py * mult + ay, w2 * mult, ph * mult };
            SDL_RenderFillRect(renderer, &sr);
        }
    }

    // Peak hold marker — thin vertical line (dB scaled)
    if (peakHold_ > 0.01f) {
        float holdDb = 20.0f * log10f(peakHold_);
        const float dbMin = -48.0f;
        const float dbMax = 6.0f;
        float holdFrac = (holdDb - dbMin) / (dbMax - dbMin);
        if (holdFrac < 0.0f) holdFrac = 0.0f;
        if (holdFrac > 1.0f) holdFrac = 1.0f;
        int holdPx = (int)(holdFrac * pw);
        if (holdPx > pw - 1) holdPx = pw - 1;
        SDL_SetRenderDrawColor(renderer, 0xDD, 0xDD, 0xDD, 0xFF);
        SDL_Rect holdRect = { (px + holdPx) * mult + ax, py * mult + ay, 2 * mult, ph * mult };
        SDL_RenderFillRect(renderer, &holdRect);
    }
}

void RecordSampleDialog::drawRecordingPhase() {
    int width = 30;
    int height = 9;
    if (width != lastWindowWidth_ || height != lastWindowHeight_) {
        SetWindow(width, height);
        lastWindowWidth_ = width;
        lastWindowHeight_ = height;
    }

    GUITextProperties props;
    SetColor(CD_NORMAL);

    // Title + Time on same line, pad to fill width
    int seconds = 0;
    if (captureSpec_.freq > 0) {
        seconds = recordedSamples_ / captureSpec_.freq;
    }
    int mins = seconds / 60;
    int secs = seconds % 60;
    char headerBuf[40];
    snprintf(headerBuf, sizeof(headerBuf), " Recording%*s%d:%02d ",
             width - 16, "", mins, secs);
    DrawString(0, 0, headerBuf, props);

    // Level label
    DrawString(1, 2, "Level:", props);

    // Reserve meter area with blank spaces — DrawGraphics() paints over these
    int meterW = width - 2;
    char meterBlank[40];
    memset(meterBlank, ' ', meterW);
    meterBlank[meterW] = 0;
    DrawString(1, 3, meterBlank, props);
    DrawString(1, 4, meterBlank, props);

    // Info - pad to full width
    char infoBuf[40];
    snprintf(infoBuf, sizeof(infoBuf), "%-*s", width - 2, "");
    snprintf(infoBuf, sizeof(infoBuf), "%dHz %s %dk",
             captureSpec_.freq,
             captureSpec_.channels == 1 ? "M" : "St",
             recordedSamples_ / 1000);
    // Pad the rest
    int ilen = (int)strlen(infoBuf);
    if (ilen < width - 2) {
        memset(infoBuf + ilen, ' ', width - 2 - ilen);
        infoBuf[width - 2] = 0;
    }
    DrawString(1, 5, infoBuf, props);

    // Stop button
    props.invert_ = true;
    DrawString(width / 2 - 2, 7, "Stop", props);
    props.invert_ = false;
}

void RecordSampleDialog::drawSavePhase() {
    int width = 34;
    int height = keyboardMode_ ? 14 : 7;
    if (width != lastWindowWidth_ || height != lastWindowHeight_) {
        SetWindow(width, height);
        lastWindowWidth_ = width;
        lastWindowHeight_ = height;
    }

    GUITextProperties props;
    SetColor(CD_NORMAL);

    // Title — pad to full width to clear stale text
    int seconds = captureSpec_.freq > 0 ? recordedSamples_ / captureSpec_.freq : 0;
    char titleBuf[48];
    snprintf(titleBuf, sizeof(titleBuf), "Save Recording (%ds)", seconds);
    int tlen = (int)strlen(titleBuf);
    if (tlen < width - 1) {
        memset(titleBuf + tlen, ' ', width - 1 - tlen);
        titleBuf[width - 1] = 0;
    }
    DrawString(1, 0, titleBuf, props);

    // Filename
    int nameY = 2;
    SetColor(CD_NORMAL);
    DrawString(1, nameY, "Name:", props);

    char buffer[2];
    buffer[1] = 0;
    for (int i = 0; i < REC_MAX_NAME; i++) {
        props.invert_ = (i == currentChar_ && selected_ == 0);
        buffer[0] = name_[i];
        DrawString(7 + i, nameY, buffer, props);
    }
    SetColor(CD_NORMAL);
    props.invert_ = false;
    DrawString(7 + REC_MAX_NAME, nameY, ".wav", props);

    // Keyboard
    if (keyboardMode_) {
        SetColor(CD_NORMAL);
        int kbY = nameY + 2;
        int kbX = 7;
        for (int row = 0; row < KEYBOARD_ROWS; row++) {
            const char *rowStr = keyboardLayout[row];
            int len = strlen(rowStr);
            int startX = kbX + (REC_MAX_NAME - len) / 2;

            if (row == SPACE_ROW) {
                props.invert_ = (row == keyboardRow_ && isInSpaceSection(keyboardCol_));
                DrawString(startX, kbY + row, "[_]", props);
                props.invert_ = (row == keyboardRow_ && isInBackSection(keyboardCol_));
                DrawString(startX + 4, kbY + row, "<-", props);
                props.invert_ = (row == keyboardRow_ && isInDoneSection(keyboardCol_));
                DrawString(startX + 8, kbY + row, "OK", props);
            } else {
                for (int col = 0; col < len; col++) {
                    props.invert_ = (row == keyboardRow_ && col == keyboardCol_);
                    buffer[0] = rowStr[col];
                    DrawString(startX + col, kbY + row, buffer, props);
                }
            }
        }
        props.invert_ = false;
        SetColor(CD_NORMAL);
        DrawString(kbX, kbY + KEYBOARD_ROWS, "A=char B=del L/R=move", props);
        return;
    }

    // Clear button row to avoid artifacts
    int btnY = height - 1;
    char clearBuf[48];
    memset(clearBuf, ' ', width);
    clearBuf[width] = 0;
    SetColor(CD_NORMAL);
    DrawString(0, btnY, clearBuf, props);

    // Buttons: Preview, Save, Discard — evenly spaced
    // selected_ 1=Preview 2=Save 3=Discard
    SetColor(CD_NORMAL);
    props.invert_ = (selected_ == 1);
    if (selected_ == 1) SetColor(CD_HILITE2);
    if (isPreviewing_) {
        DrawString(3, btnY, "Stop", props);
    } else {
        DrawString(2, btnY, "Preview", props);
    }
    props.invert_ = false;
    SetColor(CD_NORMAL);
    props.invert_ = (selected_ == 2);
    if (selected_ == 2) SetColor(CD_HILITE2);
    DrawString(14, btnY, "Save", props);
    props.invert_ = false;
    SetColor(CD_NORMAL);
    props.invert_ = (selected_ == 3);
    if (selected_ == 3) SetColor(CD_HILITE2);
    DrawString(23, btnY, "Discard", props);
    props.invert_ = false;
}

// ---- Events ----

void RecordSampleDialog::OnPlayerUpdate(PlayerEventType, unsigned int currentTick) {
    // Audio polling is handled by pollAudio() called from DrawView().
    // If the player happens to be running, just mark dirty so we redraw.
    if (phase_ == RP_RECORDING && isRecording_) {
        isDirty_ = true;
    }
}

void RecordSampleDialog::OnFocus() {
    enumerateInputDevices();
    phase_ = RP_SETUP;
    lastWindowWidth_ = 0;
    lastWindowHeight_ = 0;
    selected_ = 0;
}

// ---- Input ----

void RecordSampleDialog::ProcessButtonMask(unsigned short mask, bool pressed) {
    if (!pressed) return;

    switch (phase_) {
    case RP_SETUP: {
        if (mask == EPBM_UP || mask == EPBM_DOWN) {
            // Two groups: input row (0) and button row (1,2)
            if (selected_ == 0) {
                selected_ = 1; // jump to first button
            } else {
                selected_ = 0; // jump back to input
            }
            isDirty_ = true;
        } else if (mask == EPBM_LEFT || mask == EPBM_RIGHT) {
            int dir = (mask == EPBM_RIGHT) ? 1 : -1;
            if (selected_ == 0) {
                // Change input device
                currentDevice_ += dir;
                if (currentDevice_ < 0)
                    currentDevice_ = (int)inputDevices_.size() - 1;
                if (currentDevice_ >= (int)inputDevices_.size())
                    currentDevice_ = 0;
                probeDeviceCapabilities();
            } else {
                // Navigate between Record (1) and Cancel (2)
                selected_ += dir;
                if (selected_ < 1) selected_ = 2;
                if (selected_ > 2) selected_ = 1;
            }
            isDirty_ = true;
        } else if (mask == EPBM_A) {
            if (selected_ == 1) {
                startRecording();
            } else if (selected_ == 2) {
                EndModal(0);
            }
            isDirty_ = true;
        } else if (mask == EPBM_B) {
            EndModal(0);
            isDirty_ = true;
        }
        break;
    }

    case RP_RECORDING: {
        if (mask == EPBM_A || mask == EPBM_B) {
            stopRecording();
            isDirty_ = true;
        }
        break;
    }

    case RP_SAVE: {
        // Keyboard mode
        if (keyboardMode_) {
            if (mask == EPBM_A) {
                char ch = getKeyAtPosition(keyboardRow_, keyboardCol_);
                if (ch == '\b') {
                    if (currentChar_ > 0) {
                        currentChar_--;
                        name_[currentChar_] = ' ';
                    }
                } else if (ch == '\r') {
                    keyboardMode_ = false;
                    lastWindowWidth_ = 0;
                    lastWindowHeight_ = 0;
                } else if (ch != '\0') {
                    name_[currentChar_] = ch;
                    if (currentChar_ < REC_MAX_NAME - 1) {
                        currentChar_++;
                        findCharacterInKeyboard(name_[currentChar_],
                                                keyboardRow_, keyboardCol_);
                    }
                }
                isDirty_ = true;
                return;
            } else if (mask == EPBM_B) {
                if (currentChar_ > 0) {
                    currentChar_--;
                    name_[currentChar_] = ' ';
                }
                isDirty_ = true;
                return;
            } else if (mask == EPBM_L) {
                moveCursor(-1);
                isDirty_ = true;
                return;
            } else if (mask == EPBM_R) {
                moveCursor(1);
                isDirty_ = true;
                return;
            } else if (mask == EPBM_UP) {
                keyboardRow_ = (keyboardRow_ - 1 + KEYBOARD_ROWS) % KEYBOARD_ROWS;
                clampKeyboardColumn(keyboardRow_, keyboardCol_);
                isDirty_ = true;
                return;
            } else if (mask == EPBM_DOWN) {
                keyboardRow_ = (keyboardRow_ + 1) % KEYBOARD_ROWS;
                clampKeyboardColumn(keyboardRow_, keyboardCol_);
                isDirty_ = true;
                return;
            } else if (mask == EPBM_LEFT) {
                cycleKeyboardColumn(keyboardRow_, -1, keyboardCol_);
                isDirty_ = true;
                return;
            } else if (mask == EPBM_RIGHT) {
                cycleKeyboardColumn(keyboardRow_, 1, keyboardCol_);
                isDirty_ = true;
                return;
            } else if (mask == EPBM_START) {
                keyboardMode_ = false;
                lastWindowWidth_ = 0;
                lastWindowHeight_ = 0;
                isDirty_ = true;
                return;
            }
            return;
        }

        {
            // Name / buttons
            if (mask == EPBM_UP || mask == EPBM_DOWN) {
                // Two groups: name (0) and button row (1,2,3)
                if (selected_ == 0) {
                    selected_ = 2; // jump to Save button
                } else {
                    selected_ = 0; // jump back to name
                }
                isDirty_ = true;
            } else if (mask == EPBM_LEFT || mask == EPBM_RIGHT) {
                int dir = (mask == EPBM_RIGHT) ? 1 : -1;
                if (selected_ >= 1) {
                    // Navigate between Preview(1), Save(2), Discard(3)
                    selected_ += dir;
                    if (selected_ < 1) selected_ = 3;
                    if (selected_ > 3) selected_ = 1;
                }
                isDirty_ = true;
            } else if (mask == EPBM_A) {
                switch (selected_) {
                case 0:
                    keyboardMode_ = !keyboardMode_;
                    lastWindowWidth_ = 0; // force SetWindow for size change
                    lastWindowHeight_ = 0;
                    if (keyboardMode_) {
                        findCharacterInKeyboard(name_[currentChar_],
                                                keyboardRow_, keyboardCol_);
                    }
                    break;
                case 1:
                    // Preview / Stop
                    if (isPreviewing_) {
                        stopPreview();
                    } else {
                        startPreview();
                    }
                    break;
                case 2:
                    {
                        stopPreview();
                        bool blank = true;
                        for (int i = 0; i < REC_MAX_NAME; i++) {
                            if (name_[i] != ' ') { blank = false; break; }
                        }
                        if (!blank) {
                            saveRecording();
                            EndModal(1);
                        }
                    }
                    break;
                case 3:
                    stopPreview();
                    EndModal(0);
                    break;
                }
                isDirty_ = true;
            } else if (mask == EPBM_B) {
                stopPreview();
                EndModal(0);
                isDirty_ = true;
            }
        }
        break;
    }
    } // switch phase
}

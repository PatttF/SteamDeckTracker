#ifndef _RECORD_SAMPLE_DIALOG_H_
#define _RECORD_SAMPLE_DIALOG_H_

#include "Application/Views/BaseClasses/ModalView.h"
#include "Application/Utils/KeyboardLayout.h"
#include "UIFramework/BasicDatas/GUIEvent.h"
#include <SDL2/SDL.h>
#include <string>
#include <vector>

#define REC_MAX_NAME 20
#define REC_MAX_SECONDS 120
#define REC_FOLDER_LIST_SIZE 10
#define REC_METER_WIDTH 64

enum RecordPhase {
    RP_SETUP,      // choosing input, quality, ready to record
    RP_RECORDING,  // actively capturing audio
    RP_SAVE        // recording done, naming & folder selection
};

class RecordSampleDialog : public ModalView {
    friend void previewAudioCallback(void *userdata, Uint8 *stream, int len);
public:
    RecordSampleDialog(View &view);
    virtual ~RecordSampleDialog();

    virtual void DrawView();
    virtual void OnPlayerUpdate(PlayerEventType, unsigned int currentTick);
    virtual void OnFocus();
    virtual void ProcessButtonMask(unsigned short mask, bool pressed);
    virtual void DrawGraphics();

    // Get the full path of the saved WAV file (valid after EndModal(1))
    std::string GetSavedPath() { return savedPath_; }

private:
    // Setup phase
    void enumerateInputDevices();
    void probeDeviceCapabilities();
    void openCaptureDevice();
    void closeCaptureDevice();
    void startRecording();
    void stopRecording();

    // Preview playback
    void startPreview();
    void stopPreview();

    // Save phase
    void saveRecording();
    void moveCursor(int direction);

    // Drawing helpers
    void drawSetupPhase();
    void drawRecordingPhase();
    void drawSavePhase();

    // Audio polling (called from DrawView to dequeue captured audio)
    void pollAudio();

    // SDL timer for periodic redraws during recording/preview
    static Uint32 recordTimerCallback(Uint32 interval, void *param);

    // Phase & UI state
    RecordPhase phase_;
    int selected_;

    // Input devices
    std::vector<std::string> inputDevices_;
    int currentDevice_;

    // Quality settings
    int sampleRateIdx_;
    int channelIdx_;        // 0=mono, 1=stereo

    static const int sampleRates_[];
    static const int numSampleRates_;

    // SDL capture state
    SDL_AudioDeviceID captureDeviceId_;
    SDL_AudioSpec captureSpec_;

    // Recording buffer
    std::vector<short> recordBuffer_;
    int recordedSamples_;
    float peakLevel_;
    bool isRecording_;

    // Level meter peak hold
    float peakHold_;       // peak hold value (decays slowly)
    int peakHoldTimer_;    // frames since last peak hit

    // SDL timer ID for periodic redraws
    SDL_TimerID recordTimerID_;

    // Preview playback state
    SDL_AudioDeviceID previewDeviceId_;
    int previewPos_;       // current playback position in frames
    bool isPreviewing_;

    // Save phase - name entry
    char name_[REC_MAX_NAME + 1];
    int currentChar_;
    bool keyboardMode_;
    int keyboardRow_;
    int keyboardCol_;

    // Result
    std::string savedPath_;

    // Window size cache to avoid ClearRect+border redraw flicker
    int lastWindowWidth_;
    int lastWindowHeight_;

    // Probed device native capabilities
    int nativeRate_;
    int nativeChannels_;
};

#endif

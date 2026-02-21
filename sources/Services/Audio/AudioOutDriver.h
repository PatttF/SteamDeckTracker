#ifndef _AUDIO_OUT_DRIVER_H_
#define _AUDIO_OUT_DRIVER_H_

#include "AudioOut.h"
#include "Foundation/Observable.h"
#include "Application/Instruments/WavFileWriter.h"
#include <atomic>

class AudioDriver ;

#define MIX_BUFFER_SIZE 40000
#define MAX_POSITIVE_FIXED i2fp(32767)
#define MAX_NEGATIVE_FIXED i2fp(-32768)

struct SoftClipData {
	float alpha;
	float alpha23;
	float alphaInv;
	float gainCmp;
};

class AudioOutDriver: public AudioOut,protected I_Observer {
  public:
    AudioOutDriver(AudioDriver &) ;
    virtual ~AudioOutDriver() ;

    virtual bool Init() ;
    virtual void Close() ;
    virtual bool Start() ;
    virtual void Stop() ;

    virtual void Trigger() ;
    virtual void SetSoftclip(int clip, int gain);
    virtual void SetMasterVolume(int volume);

    virtual bool Clipped() ;

    virtual int GetPlayedBufferPercentage() ;

    AudioDriver *GetDriver() ;

    // Last final peak after master damp (0.0 .. 1.0)
    virtual float GetFinalPeak();

  private:
    std::atomic<float> finalLastPeak_ ;
    bool shuttingDown_ ;

    virtual std::string GetAudioAPI() ;
    virtual std::string GetAudioDevice() ;
    virtual int GetAudioBufferSize() ;
    virtual int GetAudioRequestedBufferSize() ;
    virtual int GetAudioPreBufferCount() ;
    virtual double GetStreamTime() ;

  protected:

    virtual void Update(Observable &o,I_ObservableData *d) ;

    void prepareMixBuffers() ;
    void mixToPrimary() ;
    void clipToMix() ;
    fixed hardClip(fixed sample);
    fixed softClip(fixed sample);

  private:
    AudioDriver *driver_ ;
    bool clipped_ ;
    bool hasSound_ ;
    int softclip_ ;
    int softclipGain_;
    int masterVolume_;

	SoftClipData softClipData_[4];

    fixed *primarySoundBuffer_ ;
    short *mixBuffer_ ;
    int sampleCount_ ;
    int cachedVolume_ ;       // Cached master volume for damp calculation
    float cachedDamp_ ;       // Cached damp factor (masterVolume^4)
    fixed cachedDampFixed_ ;  // Cached damp as fixed-point (avoids per-sample float conversion)
} ;
#endif

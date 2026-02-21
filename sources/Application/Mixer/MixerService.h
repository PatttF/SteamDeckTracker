#ifndef _MIXER_SERVICE_H_
#define _MIXER_SERVICE_H_

#ifdef SDL2
#include <SDL2/SDL.h>
#else
#include <SDL/SDL.h>
#endif
#include "Application/Commands/CommandDispatcher.h" // Would be better done externally and call an API here
#include "Application/Model/Song.h" // For SONG_CHANNEL_COUNT
#include "Foundation/Observable.h"
#include "Foundation/T_Singleton.h"
#include "Services/Audio/AudioMixer.h"
#include "Services/Audio/AudioOut.h"
#include "MixBus.h"
#include "Reverb.h"
#include <atomic>

enum MixerServiceRenderMode {
    MSRM_PLAYBACK,
    MSRM_STEREO,
    MSRM_STEMS,
};

#define MAX_BUS_COUNT 16

class MixerService: 
      public T_Singleton<MixerService>,
      public Observable,
      public I_Observer,
      public CommandExecuter      
{

public:
	MixerService() ;
	virtual ~MixerService() ;

	bool Init() ;
	void Close() ;

	bool Start() ;
	void Stop() ;

	MixBus *GetMixBus(int i) ;

	virtual void Update(Observable &o,I_ObservableData *d) ;

	void OnPlayerStart() ;
	void OnPlayerStop() ;

	bool Clipped() ;
    void SetPregain(int);
    void SetSoftclip(int, int);
    void SetMasterVolume(int);
    int GetMasterVolume();
    void SetRenderMode(int);
    bool IsRendering();
    int GetPlayedBufferPercentage() ;
	float GetMasterPeak();
	virtual void Execute(FourCC id,float value) ;

	AudioOut *GetAudioOut() ;

	void Lock() ;
	void Unlock() ;

	// Waveform buffer (recent master peaks)
	void PushWaveSample(float v);
	float GetWaveSample(int index) const; // 0..GetWaveSampleCount()-1 (oldest..newest)
	int GetWaveSampleCount() const;

	// Oscilloscope buffer (signed audio samples for real-time waveform display)
	void PushOscilloscopeSamples(const float *samples, int count);
	float GetOscilloscopeSample(int index) const; // 0..GetOscilloscopeSampleCount()-1
	int GetOscilloscopeSampleCount() const;

	// Mixer control helpers
	void SetChannelVolume(int channel, int percent); // 0..100
	int GetChannelVolume(int channel);
	void SetChannelPan(int channel, int pan); // -50..50
	int GetChannelPan(int channel);
	void ToggleChannelMute(int channel);
	void ToggleChannelSolo(int channel);

	// Reverb control
	void SetReverbSend(int channel, float amount); // 0.0 to 1.0
	float GetReverbSend(int channel);
	void SetReverbDecay(float decay); // 0.0 to 1.0 (maps to 0.0 to 0.95 internal)
	void SetReverbDamping(float damping); // 0.0 to 1.0
	Reverb &GetReverb() { return reverb_; }
	
	// Per-channel reverb send levels (set via REVB command)
	float GetChannelReverbSend(int channel);
	void SetChannelReverbSend(int channel, float send);

protected:
	void toggleRendering(bool enable) ;
private:
  void initRendering(MixerServiceRenderMode);
  AudioOut *out_;
  MixBus master_;
  MixBus bus_[MAX_BUS_COUNT];
  MixerServiceRenderMode mode_;
  SDL_mutex *sync_;
  bool isRendering_;
  int pregain_;
  int masterVolume_;

  // Waveform buffer (peak envelope)
  static const int WAVE_SAMPLES_CONST = 512;
  float waveform_[WAVE_SAMPLES_CONST];
  std::atomic<int> waveformPos_;

  // Oscilloscope buffer (signed audio samples for waveform display)
  static const int SCOPE_SAMPLES = 640; // enough for full pixel width
  float scopeBuffer_[SCOPE_SAMPLES];
  std::atomic<int> scopeWritePos_;
  
  // Reverb effect
  Reverb reverb_;
  float channelReverbSend_[SONG_CHANNEL_COUNT]; // Per-channel reverb send (0.0 to 1.0)

  // Pre-cached fixed-point constant: fl2fp(0.01f)
  fixed fpOnePercent_;
} ;
#endif
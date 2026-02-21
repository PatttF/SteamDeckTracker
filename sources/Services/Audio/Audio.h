
#ifndef _AUDIO_H_
#define _AUDIO_H_

#include "Foundation/T_Factory.h"
#include "Foundation/T_SimpleList.h"
#include "AudioOut.h"
#include "AudioSettings.h"


class Audio:public T_Factory<Audio>,public T_SimpleList<AudioOut> {
public:
	Audio(AudioSettings &settings) ;
	virtual ~Audio() ;
	virtual void Init()=0 ;
	virtual void Close()=0 ;
	virtual int GetSampleRate() { return settings_.sampleRate_ > 0 ? settings_.sampleRate_ : AUDIO_DEFAULT_SAMPLE_RATE ; } ;
	virtual int GetChannelCount() { return settings_.channelCount_ > 0 ? settings_.channelCount_ : 2 ; } ;
	virtual int GetMixerVolume() { return 100 ; } ;
	virtual void SetMixerVolume(int volume) {} ;

	// Update the actual obtained sample rate (called by driver after device opens)
	void SetActualSampleRate(int rate) { settings_.sampleRate_ = rate ; } ;
	void SetActualChannelCount(int ch) { settings_.channelCount_ = ch ; } ;

	const char *GetAudioAPI() ;
	const char *GetAudioDevice() ;
	int GetAudioBufferSize() ;
	int GetAudioPreBufferCount() ;

protected:
	AudioSettings settings_ ;

private:
	std::string audioAPI_ ;
	std::string audioDevice_ ;
	int audioBufferSize_ ;
	int preBufferCount_ ;
} ;
#endif

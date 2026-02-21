#ifndef _AUDIO_MIXER_H_
#define _AUDIO_MIXER_H_

#include "AudioModule.h"
#include "Foundation/T_SimpleList.h"
#include "Application/Instruments/WavFileWriter.h"
#include <string>
#include <atomic>

class AudioMixer: public AudioModule,public T_SimpleList<AudioModule> {
public:
	AudioMixer(const char *name) ;
	virtual ~AudioMixer() ;
	virtual bool Render(fixed *buffer,int samplecount) ;
	void SetFileRenderer(const char *path) ;
	void EnableRendering(bool enable) ;
	void SetVolume(fixed volume) ;
	// Live peak meter (0.0 .. 1.0)
	float GetLastPeak() const { return lastPeak_.load(std::memory_order_relaxed); }
private:
	bool enableRendering_ ;
	std::string renderPath_ ;
	WavFileWriter *writer_ ;
	fixed volume_ ;
	std::string name_ ; 
	std::atomic<float> lastPeak_ ;
	// Pre-allocated mix buffer to avoid malloc/free in audio callback
	fixed *mixBuffer_ ;
	int mixBufferSize_ ;
} ;
#endif

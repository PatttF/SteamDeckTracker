#ifndef _SDL_SOUND_H_
#define _SDL_SOUND_H_

#include "Services/Audio/AudioDriver.h"
#include <SDL2/SDL.h>
#include "System/Process/Process.h"

class SDLAudioDriver ;

class SDLAudioDriverThread: public SysThread {
public:
	SDLAudioDriverThread(SDLAudioDriver *driver) ;
	virtual ~SDLAudioDriverThread() {} ;
	virtual bool Execute() ;
	virtual void RequestTermination() ;
	void Notify() ;
	int GetPlayedBufferPercentage() { return percentage_; };
private:
	SDLAudioDriver *driver_ ;
	SysSemaphore *semaphore_ ;
	int percentage_ ;
} ;

class SDLAudioDriver:public AudioDriver {
public:
  SDLAudioDriver(AudioSettings &settings) ;
  virtual ~SDLAudioDriver() ;
  
   // Sound implementation
	virtual bool InitDriver() ; 
	virtual void CloseDriver();
	virtual bool StartDriver() ; 
	virtual void StopDriver();
	virtual int GetPlayedBufferPercentage() ;
	virtual int GetSampleRate() { return sampleRate_ ; } ;
	virtual int GetChannelCount() { return channelCount_ ; } ;
	virtual bool Interlaced() { return true ; } ;	
	virtual double GetStreamTime() ;
	// Additional
	void OnChunkDone(Uint8 *stream,int len) ;

private:
  int fragSize_ ;  // Actual fragsize used by the driver
  char *unalignedMain_ ;
  char *mainBuffer_ ;
  char *miniBlank_ ;
  int bufferPos_ ;
  int bufferSize_ ;
  int sampleRate_ ;   // Actual obtained sample rate from audio device
  int channelCount_ ; // Actual obtained channel count from audio device
  SDL_AudioDeviceID audioDeviceId_ ; // Modern SDL2 audio device handle
  SDLAudioDriverThread *thread_ ;
	Uint32 startTime_ ;
} ;


#endif

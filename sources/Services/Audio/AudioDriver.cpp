
#include "AudioDriver.h"
#include "System/System/System.h"
#include "System/Console/Trace.h"
#include "System/Console/n_assert.h"

AudioDriver::AudioDriver(AudioSettings &settings) {
	settings_=settings ;
}

AudioDriver::~AudioDriver() {
	for (int i=0;i<SOUND_BUFFER_COUNT;i++) {
		SAFE_FREE(pool_[i].buffer_) ;
	}
}

bool AudioDriver::Init() {

  // Clear all buffers
	
   for (int i=0;i<SOUND_BUFFER_COUNT;i++) {
     pool_[i].buffer_=0 ;
     pool_[i].size_=0 ;
   } ;
   isPlaying_=false;	 

   return InitDriver() ;
}

void AudioDriver::Close() {
	CloseDriver() ;
};

bool AudioDriver::Start() {

    isPlaying_=true ; 

    // Mark all pool slots as empty but keep allocations for reuse
    for (int i=0;i<SOUND_BUFFER_COUNT;i++) {
  	  pool_[i].size_=0 ;
    } ;
	 
    poolQueuePosition_=0 ;
    poolPlayPosition_=0 ;
	hasData_=false ;

    return StartDriver() ;
};

void AudioDriver::Stop() {
     isPlaying_=false ;
	hasData_=false ;
     StopDriver() ;
}

void AudioDriver::AddBuffer(short *buffer,int samplecount) {
  
  int len=samplecount*2*sizeof(short) ;

  if (!isPlaying_) return ;

  if (len>SOUND_BUFFER_MAX) {
      Trace::Error("Alert: buffer size exceeded") ;
      return ;
  }

  // size_>0 means the slot still has unconsumed data
  if (pool_[poolQueuePosition_].size_>0) {
  // Resync: snap the queue position to the play position (where the
  // consumer just freed a slot) instead of blindly advancing.  This
  // prevents the permanent desynchronisation that occurs when an IPC
  // stall (e.g. yabridge/Wine) floods the semaphore and the producer
  // burns through all pool slots in one burst.
  int candidate=poolPlayPosition_ ;
  bool found=false ;
  for (int i=0;i<SOUND_BUFFER_COUNT;i++) {
    if (pool_[candidate].size_==0) { found=true; break; }
    candidate=(candidate+1)%SOUND_BUFFER_COUNT ;
  }
  if (found) {
    poolQueuePosition_=candidate ;
    // Fall through to write the buffer at the recovered position
  } else {
    Trace::Error("Audio overrun — pool full, dropping buffer") ;
    return ;
  }
  }

  // Pre-allocate buffer on first use, reuse thereafter (avoid malloc/free in audio path)
  if (pool_[poolQueuePosition_].buffer_==0) {
      pool_[poolQueuePosition_].buffer_=(char*)SYS_MALLOC(SOUND_BUFFER_MAX) ;
  }

  SYS_MEMCPY(pool_[poolQueuePosition_].buffer_,(char *)buffer,len) ;
  pool_[poolQueuePosition_].size_=len ;
  poolQueuePosition_=(poolQueuePosition_+1)%SOUND_BUFFER_COUNT ;
	hasData_=true ;
}

void AudioDriver::OnNewBufferNeeded() {
  SetChanged() ;
  Event event(Event::ADET_BUFFERNEEDED);
  NotifyObservers(&event) ;
} ;

void AudioDriver::onAudioBufferTick()
{
  SetChanged() ;
  Event event(Event::ADET_DRIVERTICK);
  NotifyObservers(&event) ;
}

bool AudioDriver::hasData() {
	return hasData_ ;
}  ;

AudioSettings AudioDriver::GetAudioSettings() {
	return settings_ ;
} ;

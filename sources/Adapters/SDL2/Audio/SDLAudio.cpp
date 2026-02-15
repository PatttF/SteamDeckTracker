
#include "SDLAudio.h"
#include "SDLAudioDriver.h"
#include "Services/Audio/AudioOutDriver.h"
#include "Application/Model/Config.h"

SDLAudio::SDLAudio(AudioSettings &hints):Audio(hints) {
	hints_=hints;
}

SDLAudio::~SDLAudio() {
}

void SDLAudio::Init() {
	AudioSettings settings ;
	settings.audioAPI_=GetAudioAPI();

	settings.bufferSize_=GetAudioBufferSize() ;
	settings.preBufferCount_=GetAudioPreBufferCount() ;
	settings.sampleRate_=AUDIO_DEFAULT_SAMPLE_RATE ;

   // Check config for explicit sample rate override
   Config *config=Config::GetInstance() ;
   const char *rateStr=config->GetValue("AUDIOSAMPLERATE") ;
   if (rateStr) {
       int rate = atoi(rateStr) ;
       if (rate > 0) settings.sampleRate_ = rate ;
   }

   SDLAudioDriver *drv=new SDLAudioDriver(settings) ;
   AudioOut *out=new AudioOutDriver(*drv) ;
   Insert(out) ;

   // After driver init, propagate actual obtained sample rate back to Audio
   // so all consumers via Audio::GetInstance()->GetSampleRate() get the real value
   int actualRate = drv->GetSampleRate() ;
   int actualChannels = drv->GetChannelCount() ;
   SetActualSampleRate(actualRate) ;
   SetActualChannelCount(actualChannels) ;
   
} ;

void SDLAudio::Close() {
     IteratorPtr<AudioOut>it(GetIterator()) ;
     for (it->Begin();!it->IsDone();it->Next()) {
         AudioOut &current=it->CurrentItem() ;
         current.Close() ;
     }
} ;

int SDLAudio::GetMixerVolume() {
	return 100 ;
} ;

void SDLAudio::SetMixerVolume(int volume) {
} ;

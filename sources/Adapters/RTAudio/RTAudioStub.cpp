
#include "RTAudioStub.h"
#include "RTAudioDriver.h"
#include "Services/Audio/AudioOutDriver.h"
#include "System/Console/Trace.h"
#include <map>


RTAudioStub::RTAudioStub(AudioSettings &h):Audio(h),
	sampleRate_(0) 
{

  std::map<int, std::string> apiMap;
  apiMap[RtAudio::MACOSX_CORE] = "OS-X Core Audio";
  apiMap[RtAudio::WINDOWS_ASIO] = "Windows ASIO";
  apiMap[RtAudio::WINDOWS_DS] = "Windows Direct Sound";
  apiMap[RtAudio::UNIX_JACK] = "Jack Client";
  apiMap[RtAudio::LINUX_ALSA] = "Linux ALSA";
  apiMap[RtAudio::LINUX_OSS] = "Linux OSS";
  apiMap[RtAudio::RTAUDIO_DUMMY] = "RtAudio Dummy";

  std::vector< RtAudio::Api > apis;
  RtAudio :: getCompiledApi( apis );
#ifdef __LINUX_ALSA__
  api_=RtAudio::LINUX_ALSA;
  if (!strcmp(GetAudioAPI(),"OSS")) {
	  api_=RtAudio::LINUX_OSS ; 
  } 
#else
#ifdef __MACOSX_CORE__
  api_=RtAudio::MACOSX_CORE;
#else
  api_=RtAudio::WINDOWS_DS;
  if (!strcmp(GetAudioAPI(),"ASIO")) {
	  api_=RtAudio::WINDOWS_ASIO ; 
  } 
#endif
#endif

  RtAudio audio(api_) ;


  

  unsigned int devices = audio.getDeviceCount();

  std::string defaultDevice ;

  std::string name=GetAudioDevice() ; 
  const char *deviceName=(name.size()!=0)?name.c_str():0 ;

  RtAudio::DeviceInfo selinfo ;
  for (unsigned int i=0; i<devices; i++) 
  {
    RtAudio::DeviceInfo info = audio.getDeviceInfo(i);
    if (info.outputChannels>0)
    {
      if (info.isDefaultOutput || defaultDevice.length() ==0)
      {
        defaultDevice=info.name ;
        if (!selinfo.probed) selinfo=info ;
      }

      if ((deviceName)&&(!strncmp(deviceName,info.name.c_str(),strlen(deviceName))))
      {
        selectedDevice_=info.name ;
        selinfo=info ;
      }
      
    }
  }
  if (selectedDevice_.length()==0)
  {
    selectedDevice_=defaultDevice ;
  }
  

/*  if ( selinfo.probed == false )
	  
    else {
      
      
      
      
      if ( selinfo.isDefaultOutput ) 
      else 
      if ( selinfo.isDefaultInput ) 
      else 
      if ( selinfo.nativeFormats == 0 )
        
      else {
        
        if ( selinfo.nativeFormats & RTAUDIO_SINT8 )
          
        if ( selinfo.nativeFormats & RTAUDIO_SINT16 )
          
        if ( selinfo.nativeFormats & RTAUDIO_SINT24 )
          
        if ( selinfo.nativeFormats & RTAUDIO_SINT32 )
          
        if ( selinfo.nativeFormats & RTAUDIO_FLOAT32 )
          
        if ( selinfo.nativeFormats & RTAUDIO_FLOAT64 )
          
      }
      if ( selinfo.sampleRates.size() < 1 )
        
      else {
        
        for (unsigned int j=0; j<selinfo.sampleRates.size(); j++)
          
      }
	}
			
	
*/	
}

RTAudioStub::~RTAudioStub() {
}

void RTAudioStub::Init() 
{
	AudioSettings settings ;
	settings.audioAPI_=GetAudioAPI();
	settings.audioDevice_=selectedDevice_;
	settings.bufferSize_=GetAudioBufferSize();
	settings.preBufferCount_=GetAudioPreBufferCount();

  RTAudioDriver *drv=new RTAudioDriver(api_,settings) ;
  AudioOutDriver *out=new AudioOutDriver(*drv) ;
  Insert(out) ;
  sampleRate_=drv->GetSampleRate() ;
}

void RTAudioStub::Close() 
{
  IteratorPtr<AudioOut>it(GetIterator()) ;
  for (it->Begin();!it->IsDone();it->Next())
  {
    AudioOut &current=it->CurrentItem() ;
    current.Close() ;
  }
}

int RTAudioStub::GetSampleRate()  
{
	return sampleRate_ ;
} ;

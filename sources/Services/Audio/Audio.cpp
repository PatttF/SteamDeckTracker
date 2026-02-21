#include "Audio.h"
#include "Application/Model/Config.h"

Audio::Audio(AudioSettings &hints):T_SimpleList<AudioOut>(true),settings_() {

	// Hints contains the basic information about the
	// default settings for the platform. All of the can
	// be overriden through the config file

	Config *config=Config::GetInstance() ;
	const char *v=config->GetValue("AUDIOAPI") ;
	settings_.audioAPI_=v?v:hints.audioAPI_ ;
	v=config->GetValue("AUDIODEVICE")  ;
	settings_.audioDevice_=v?v:hints.audioDevice_ ;
	v=config->GetValue("AUDIOBUFFERSIZE") ;
	settings_.bufferSize_=v?atoi(v):hints.bufferSize_ ;
	v=config->GetValue("AUDIOPREBUFFERCOUNT") ;
	settings_.preBufferCount_=v?atoi(v):hints.preBufferCount_ ;
	v=config->GetValue("AUDIOSAMPLERATE") ;
	settings_.sampleRate_=v?atoi(v):hints.sampleRate_ ;
	if (settings_.sampleRate_ <= 0) settings_.sampleRate_ = AUDIO_DEFAULT_SAMPLE_RATE ;
	settings_.channelCount_=hints.channelCount_ ;
	if (settings_.channelCount_ <= 0) settings_.channelCount_ = 2 ;


	
	
	
	
	
	
}

Audio::~Audio() {
}

const char *Audio::GetAudioAPI() {
	return settings_.audioAPI_.c_str() ;
}

const char *Audio::GetAudioDevice() {
	return settings_.audioDevice_.c_str() ;
} ;

int Audio::GetAudioBufferSize() {
	return settings_.bufferSize_ ;
} ;

int Audio::GetAudioPreBufferCount() {
	return settings_.preBufferCount_ ;
} ;

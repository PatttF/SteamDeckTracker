#ifndef _AUDIO_SETTINGS_H_
#define _AUDIO_SETTINGS_H_

#include <string>
// Used to propagate audio hints & settings

// Common sample rates the system may encounter
#define AUDIO_DEFAULT_SAMPLE_RATE 44100

struct AudioSettings {
	std::string audioAPI_;
	std::string audioDevice_ ;
	int bufferSize_ ;
	int preBufferCount_ ;
	int sampleRate_ ;      // Actual device sample rate (0 = use default)
	int channelCount_ ;    // Actual device channel count (0 = use default of 2)

	AudioSettings() : bufferSize_(0), preBufferCount_(0),
		sampleRate_(AUDIO_DEFAULT_SAMPLE_RATE), channelCount_(2) {}
} ;

#endif

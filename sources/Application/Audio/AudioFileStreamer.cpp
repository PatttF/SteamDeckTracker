#include "AudioFileStreamer.h"
#include "Application/Utils/fixed.h"
#include "System/Console/Trace.h"
#include "Application/Model/Config.h"

AudioFileStreamer::AudioFileStreamer() {
	wav_=0 ;
	shift_=1 ;
	mode_=AFSM_STOPPED ;
	newPath_=false ;
} ;

AudioFileStreamer::~AudioFileStreamer() {
	SAFE_DELETE(wav_) ;
} ;
 
bool AudioFileStreamer::Start(const Path &path) {
  
	path_=path ;
	const char *shift=Config::GetInstance()->GetValue("PRELISTENATTENUATION") ;
	shift_=(shift)?atoi(shift):1 ;
  
	newPath_=true ;
	mode_=AFSM_PLAYING ;
	return true ;
} ;

void AudioFileStreamer::Stop() {
	mode_=AFSM_STOPPED ;
  
} ;

bool AudioFileStreamer::Render(fixed *buffer,int samplecount) {

	// See if we're playing

	if (mode_==AFSM_STOPPED) {
		SAFE_DELETE(wav_) ;
		return false ;
	}

	// Do we need to get a new file ?

	if (newPath_) {
		SAFE_DELETE(wav_) ;
		newPath_=false ;
	}

	// new look if we need to load the file

	if (!wav_) 
  {
		wav_=AudioFile::Open(path_.GetPath().c_str()) ;
		if (!wav_) 
    {
      Trace::Error("Failed to open streaming of %s",path_.GetPath().c_str());
			mode_=AFSM_STOPPED ;
			return false ;
		}
		position_=0 ;
	}

	// we are playing a valid file

	long size=wav_->GetSize(-1) ;
	int count=samplecount ;
	if (position_+samplecount>=size)
  {
    
		count=size-position_ ;
		mode_=AFSM_STOPPED ;
		memset(buffer,0,2*samplecount*sizeof(fixed)) ;
	}
	// AudioFile already has all samples loaded
	fixed *dst=buffer ;
	short *src=(short *)wav_->GetSampleBuffer(-1) ;
	src += position_ * wav_->GetChannelCount(-1);  // Advance to current position
	int channel=wav_->GetChannelCount(-1) ;

 // I might need to do sample interpolation here

	for (int i=0;i<count;i++) {
		fixed v=*dst++=i2fp((*src++)>>(1+shift_)) ;
		if (channel==2) {
			*dst++=i2fp((*src++)>>(1+shift_)) ;
		} else {
			*dst++=v ;
		}
	}
	position_+=count ;
	return true ;
}

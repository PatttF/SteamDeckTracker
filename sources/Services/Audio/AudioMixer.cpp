#include "AudioMixer.h"
#include "System/System/System.h"
#include <math.h>

AudioMixer::AudioMixer(const char *name):
	T_SimpleList<AudioModule>(false),
	enableRendering_(0),
	writer_(0),
	name_(name),
	lastPeak_(0.0f),
	mixBuffer_(0),
	mixBufferSize_(0)
{
	volume_=(i2fp(1)) ;
} ;

AudioMixer::~AudioMixer() {
	SAFE_FREE(mixBuffer_) ;
	mixBufferSize_ = 0 ;
}

void AudioMixer::SetFileRenderer(const char *path) {
	renderPath_=path ;
} ;

void AudioMixer::EnableRendering(bool enable) {

	if (enable==enableRendering_) {
		return ;
	}

	if (enable) {
		writer_=new WavFileWriter(renderPath_.c_str()) ;
	} 

	enableRendering_=enable ;
	if (!enable) {
		writer_->Close() ;
		SAFE_DELETE(writer_) ;
	}
} ;

bool AudioMixer::Render(fixed *buffer,int samplecount) {

     // Ensure pre-allocated mix buffer is large enough
     int requiredSize = samplecount * 2 ;
     if (requiredSize > mixBufferSize_) {
         SAFE_FREE(mixBuffer_) ;
         mixBuffer_ = (fixed *)SYS_MALLOC(requiredSize * sizeof(fixed)) ;
         mixBufferSize_ = requiredSize ;
     }

     bool gotData=false ;
     int moduleCount = 0;
     IteratorPtr<AudioModule>it(GetIterator()) ;
     for (it->Begin();!it->IsDone();it->Next()) {
         moduleCount++;
         AudioModule &current=it->CurrentItem() ;
         if (!gotData) {
            gotData=current.Render(buffer,samplecount) ;           
         } else {
            // Zero the mix buffer before rendering to prevent uninitialized data artifacts
            memset(mixBuffer_, 0, requiredSize * sizeof(fixed)) ;
            if (current.Render(mixBuffer_,samplecount)) {
               fixed *dst=buffer ;
               fixed *src=mixBuffer_ ;
               int count=samplecount*2 ;
               while (count--) {
                 // Saturating add to prevent signed overflow (UB)
                 long long sum = (long long)*dst + (long long)*src ;
                 if (sum > 0x7FFFFFFF) sum = 0x7FFFFFFF ;
                 else if (sum < (long long)(int)0x80000000) sum = (int)0x80000000 ;
                 *dst = (fixed)sum ;
                 dst++ ;
                 src++ ;
               }
            }
         }
     }
     


     //  Apply volume

     if (gotData) {
         fixed *c = buffer;
         if (volume_ != i2fp(1)) {
             for (int i = 0; i < samplecount * 2; i++) {
                 fixed v = fp_mul(*c, volume_);
                 *c++ = v;
             }
         }

         // Update lastPeak_ (0.0 .. 1.0) based on absolute sample values
         float peak = 0.0f;
         c = buffer;
         for (int i = 0; i < samplecount * 2; i++) {
             float f = fabsf(fp2fl(*c++));
             if (f > peak) peak = f;
         }
         if (peak > 1.0f) peak = 1.0f;
         lastPeak_ = peak;
     } else {
         lastPeak_ = 0.0f;
     }

    if (enableRendering_&&writer_) {
		if (!gotData) {
			memset(buffer,0,samplecount*2*sizeof(fixed)) ;
		} ;
		writer_->AddBuffer(buffer,samplecount) ;
	}
     return gotData ;
} ;

void AudioMixer::SetVolume(fixed volume) { volume_ = volume; }

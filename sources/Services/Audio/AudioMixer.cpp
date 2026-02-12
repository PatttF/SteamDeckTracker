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
     // Traverse the linked list directly to avoid heap-allocating an
     // iterator on every audio callback (new/delete = priority inversion).
     for (Node<AudioModule> *node = GetFirstNode(); node != NULL; node = node->next) {
         AudioModule &current = node->data ;
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

     //  Apply volume and compute peak in a single fused loop

     if (gotData) {
         float peak = 0.0f;
         fixed *c = buffer;
         bool applyVol = (volume_ != i2fp(1));
         for (int i = 0; i < samplecount * 2; i++) {
             fixed v = applyVol ? fp_mul(*c, volume_) : *c;
             if (applyVol) *c = v;
             float f = fabsf(fp2fl(v));
             if (f > peak) peak = f;
             c++;
         }
         // Normalize: fp2fl returns values in 0..32767 range for full-scale
         // 16-bit audio. Divide by 32768 to get 0..1 range.
         peak /= 32768.0f;
         lastPeak_.store(peak, std::memory_order_relaxed);
     } else {
         lastPeak_.store(0.0f, std::memory_order_relaxed);
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

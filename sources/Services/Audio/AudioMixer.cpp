#include "AudioMixer.h"
#include "System/System/System.h"
#include <math.h>

AudioMixer::AudioMixer(const char *name):
	T_SimpleList<AudioModule>(false),
	enableRendering_(0),
	writer_(0),
	name_(name),
	lastPeak_(0.0f)
{
	volume_=(i2fp(1)) ;
} ;

AudioMixer::~AudioMixer() {
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

     fixed *mixBuffer=0 ;
     bool gotData=false ;
     int moduleCount = 0;
     IteratorPtr<AudioModule>it(GetIterator()) ;
     for (it->Begin();!it->IsDone();it->Next()) {
         moduleCount++;
         AudioModule &current=it->CurrentItem() ;
         if (!gotData) {
            gotData=current.Render(buffer,samplecount) ;           
         } else {
            if (!mixBuffer) {
               mixBuffer=(fixed *)malloc(samplecount*2*sizeof(fixed)) ;
            } 
            if (current.Render(mixBuffer,samplecount)) {
               fixed *dst=buffer ;
               fixed *src=mixBuffer ;
               int count=samplecount*2 ;
               while (count--) {
                 *dst+=*src ;
                 dst++ ;
                 src++ ;
               }
            }
         }
     }
     
     static int debugCount = 0;
     if (gotData && debugCount++ % 200 == 0) {
         Trace::Log("AudioMixer", "%s: modules=%d gotData=%d buf[0]=%d", name_.c_str(), moduleCount, gotData, buffer[0]);
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
     SAFE_FREE(mixBuffer) ;
     
     // Debug: Always log for AudioOut to trace the exact buffer content at return
     if (gotData && name_ == "AudioOut") {
         Trace::Debug("[AudioMixer] %s RETURNING: gotData=%d buf[0]=%d buf[1]=%d", 
                      name_.c_str(), gotData, buffer[0], buffer[1]);
     }
     
     return gotData ;
} ;

void AudioMixer::SetVolume(fixed volume) { volume_ = volume; }


#include "AudioOutDriver.h"
#include "Application/Model/Project.h"
#include "Application/Player/SyncMaster.h" // Should be installable
#include "AudioDriver.h"
#include "Services/Time/TimeService.h"
#include "System/Console/Trace.h"
#include "System/System/System.h"
#include <math.h>

AudioOutDriver::AudioOutDriver(AudioDriver &driver) {
    // Initialize member variables to safe defaults
    clipped_ = false;
    hasSound_ = false;
    softclip_ = -1;  // bypass
    softclipGain_ = 0;
    masterVolume_ = 100;
    sampleCount_ = 0;

    // Precalculate constant values for softclipping algorithm
    softClipData_[0].alpha = 1.45f; // -1.5db (approx.)
	softClipData_[1].alpha = 1.07f; // -3db (approx.)
	softClipData_[2].alpha = 0.75f; // -6db (approx.)
	softClipData_[3].alpha = 0.53f; // -9db (approx.)

	for (int i = 0; i < 4; i++) {
		softClipData_[i].alpha23 = softClipData_[i].alpha * (2.0f / 3.0f);
		softClipData_[i].alphaInv = 1.0f / softClipData_[i].alpha;

		if (softClipData_[i].alpha > 1.0f) {
			/* calculates gain compensation differently for
			 * modes with alpha > 1, so there's no drop in loudness
			 * and we can still drive the hard clipper when the input
			 * goes over 1.0
			 */
			softClipData_[i].gainCmp = 1.0f / (1.0f - (pow(softClipData_[i].alphaInv, 2.0f) / 3.0f));
		} else {
			softClipData_[i].gainCmp = 1.0f / softClipData_[i].alpha23;
		}
	}

	driver_=&driver ;
	driver.AddObserver(*this) ;
	primarySoundBuffer_=0 ;
    mixBuffer_ = 0;
    finalLastPeak_ = 0.0f;
    shuttingDown_ = false;
    SetOwnership(false);
}

AudioOutDriver::~AudioOutDriver() {
    shuttingDown_ = true;
    // ensure audio thread/driver stopped before we remove the observer
    driver_->Stop();
    driver_->Close();
    driver_->RemoveObserver(*this);
    delete driver_ ;
};

bool AudioOutDriver::Init() {
	primarySoundBuffer_=(fixed *)SYS_MALLOC(MIX_BUFFER_SIZE*sizeof(fixed)/2) ;
	mixBuffer_=(short *)SYS_MALLOC(MIX_BUFFER_SIZE) ;
    return driver_->Init();
} ;

void AudioOutDriver::Close() {
    driver_->Close();
    SAFE_FREE(primarySoundBuffer_) ;
	SAFE_FREE(mixBuffer_) ;     
}

bool AudioOutDriver::Start() {
    clipped_ = false;
    sampleCount_=0 ;
	return driver_->Start() ;
}

void AudioOutDriver::Stop() {
	driver_->Stop() ;
}

bool AudioOutDriver::Clipped() { return clipped_; };

void AudioOutDriver::Trigger() {

	if (shuttingDown_) return;
	TimeService *ts=TimeService::GetInstance() ;

    prepareMixBuffers();
    hasSound_=AudioMixer::Render(primarySoundBuffer_,sampleCount_) ;
    clipToMix();
    if (!shuttingDown_) driver_->AddBuffer(mixBuffer_,sampleCount_) ;
}

void AudioOutDriver::Update(Observable &o,I_ObservableData *d) 
{
    if (shuttingDown_) return;
    SetChanged();
    NotifyObservers(d) ;
}

void AudioOutDriver::prepareMixBuffers() {
	sampleCount_=getPlaySampleCount() ; 
	clipped_=false ;  
	finalLastPeak_ = 0.0f;  
} ;

void AudioOutDriver::SetSoftclip(int clip, int gain) {
    softclip_ = clip - 1;
	softclipGain_ = gain;
}

void AudioOutDriver::SetMasterVolume(int volume) {
	masterVolume_ = volume;
}

fixed AudioOutDriver::hardClip(fixed sample) {

    if (sample > MAX_POSITIVE_FIXED || sample < MAX_NEGATIVE_FIXED) {
        clipped_ = true;
		return sample > 0 ? MAX_POSITIVE_FIXED : MAX_NEGATIVE_FIXED;
    }

    return sample;
}

/* Implements standard cubic algorithm
 * https://wiki.analog.com/resources/tools-software/sigmastudio/toolbox/nonlinearprocessors/standardcubic
 */
fixed AudioOutDriver::softClip(fixed sample) {
    if (softclip_ == -1 || sample == 0)
        return sample;

    float x;
    float sampleFloat = fp2fl(sample);
	float maxFloat = fp2fl(sampleFloat > 0 ? MAX_POSITIVE_FIXED : MAX_NEGATIVE_FIXED);
	SoftClipData* data = &softClipData_[softclip_];

    x = data->alphaInv * (sampleFloat / maxFloat);
    if (x > -1.0f && x < 1.0f) {
        sampleFloat = maxFloat * (data->alpha * (x - (pow(x, 3.0f) / 3.0f)));
    } else {
        sampleFloat = maxFloat * data->alpha23;
    }

    if (softclipGain_) {
        sampleFloat = sampleFloat * data->gainCmp;
    }

    return fl2fp(sampleFloat);
}

void AudioOutDriver::clipToMix() {

    float damp = pow((float)masterVolume_ / 100, 4.0f);
    bool interlaced = driver_->Interlaced();

    if (!hasSound_) {
        SYS_MEMSET(mixBuffer_, 0, sampleCount_ * 2 * sizeof(short));
    } else {
        short *s1 = mixBuffer_;
		short *s2 = (interlaced) ? s1 + 1 : s1 + sampleCount_;
		int offset = (interlaced) ? 2 : 1;

        fixed *p = primarySoundBuffer_;

        fixed leftSample;
        fixed rightSample;

        float finalPeak = 0.0f;
        for (int i = 0; i < sampleCount_; i++) {

            // Apply softclip first, then master damp, then hard clip to reflect final output clipping.
            fixed l = softClip(*p++);
            fixed r = softClip(*p++);

            // Apply master damp in floating point to preserve accuracy, then convert back to fixed.
            fixed l_damped = fl2fp(fp2fl(l) * damp);
            fixed r_damped = fl2fp(fp2fl(r) * damp);

            leftSample = hardClip(l_damped);
            rightSample = hardClip(r_damped);

            // Track final peak after damping/clipping
            float lf = fabsf(fp2fl(leftSample));
            float rf = fabsf(fp2fl(rightSample));
            if (lf > finalPeak) finalPeak = lf;
            if (rf > finalPeak) finalPeak = rf;

            *s1 = short(fp2i(leftSample));
            s1 += offset;
			*s2 = short(fp2i(rightSample));
			s2 += offset;
        };

        // Store final post-damp peak for UI and meters
        if (finalPeak > 1.0f) finalPeak = 1.0f;
        finalLastPeak_ = finalPeak;
    }
} ;

int AudioOutDriver::GetPlayedBufferPercentage() {
	return driver_->GetPlayedBufferPercentage() ;
} ;

float AudioOutDriver::GetFinalPeak() { return finalLastPeak_; } ;

AudioDriver *AudioOutDriver::GetDriver() { return driver_; };

std::string AudioOutDriver::GetAudioAPI() {
	AudioSettings as=driver_->GetAudioSettings() ;
	return as.audioAPI_ ;
} ;

std::string AudioOutDriver::GetAudioDevice() {
	AudioSettings as=driver_->GetAudioSettings() ;
	return as.audioDevice_ ;
} ;

int AudioOutDriver::GetAudioBufferSize() {
	AudioSettings as=driver_->GetAudioSettings() ;
	return as.bufferSize_ ;
} ;

int AudioOutDriver::GetAudioRequestedBufferSize() {
	AudioSettings as=driver_->GetAudioSettings() ;
	return as.bufferSize_ ;
}

int AudioOutDriver::GetAudioPreBufferCount() {
	AudioSettings as=driver_->GetAudioSettings() ;
	return as.preBufferCount_ ;
} ;
double AudioOutDriver::GetStreamTime() { return driver_->GetStreamTime(); };

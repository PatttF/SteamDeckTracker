
#include "AudioOutDriver.h"
#include "Application/Mixer/MixerService.h"
#include "Application/Model/Project.h"
#include "Application/Player/SyncMaster.h" // Should be installable
#include "AudioDriver.h"
#include "System/Console/Trace.h"
#include "System/System/System.h"
#include <math.h>
#include <string.h>

// Denormal protection: flush denormals to zero to prevent CPU spikes
// in IIR filters, reverbs, and plugin processing
#if defined(__SSE__) || defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#include <pmmintrin.h>
static inline void enableDenormalProtection() {
    _mm_setcsr(_mm_getcsr() | 0x8040); // FTZ + DAZ
}
#elif defined(__aarch64__) || defined(__ARM_NEON)
static inline void enableDenormalProtection() {
    uint64_t fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1 << 24); // FZ bit
    __asm__ __volatile__("msr fpcr, %0" :: "r"(fpcr));
}
#else
static inline void enableDenormalProtection() { /* no-op */ }
#endif

AudioOutDriver::AudioOutDriver(AudioDriver &driver) {
    // Initialize member variables to safe defaults
    clipped_ = false;
    hasSound_ = false;
    softclip_ = -1;  // bypass
    softclipGain_ = 0;
    masterVolume_ = 100;
    sampleCount_ = 0;
    cachedVolume_ = -1;
    cachedDamp_ = 1.0f;
    cachedDampFixed_ = FP_ONE;

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
    finalLastPeak_.store(0.0f, std::memory_order_relaxed);
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

    // Prevent denormal CPU spikes in plugin/filter processing
    enableDenormalProtection();

    prepareMixBuffers();

    // Zero the primary buffer before rendering to prevent stale data artifacts
    memset(primarySoundBuffer_, 0, sampleCount_ * 2 * sizeof(fixed));

    hasSound_=AudioMixer::Render(primarySoundBuffer_,sampleCount_) ;
    
    // NOTE: Per-channel reverb is now applied in SampleInstrument::Render()
    // The REVB command sets per-note reverb parameters
    
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
	// Clamp to max buffer capacity: primarySoundBuffer_ holds MIX_BUFFER_SIZE/2 bytes
	// = MIX_BUFFER_SIZE/(2*sizeof(fixed)) fixed elements = MIX_BUFFER_SIZE/8 stereo samples
	int maxSamples = MIX_BUFFER_SIZE / (2 * (int)sizeof(fixed));
	if (sampleCount_ > maxSamples) sampleCount_ = maxSamples;
	clipped_=false ;  
	// NOTE: Do NOT reset finalLastPeak_ here. The previous cycle's value
	// is harmless and will be overwritten by clipToMix() after Render()
	// completes. Resetting it to 0 created a race window where the GUI
	// thread could read 0 during the (potentially long) Render() call,
	// causing the master meter to show no levels for slow-rendering
	// plugins (LV2/VST3).
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
    // Caller already guards softclip_ != -1, so no check needed here
    if (sample == 0) return 0;

    // One fp2fl at entry, one fl2fp at exit
    float sampleFloat = fp2fl(sample);
    static const float maxFloat = 32767.0f; // fp2fl(MAX_POSITIVE_FIXED)
    float sign = (sampleFloat >= 0.0f) ? 1.0f : -1.0f;
    float absSample = sampleFloat * sign;
    SoftClipData* data = &softClipData_[softclip_];

    float x = data->alphaInv * (absSample / maxFloat);
    if (x < 1.0f) {
        absSample = maxFloat * (data->alpha * (x - (x * x * x / 3.0f)));
    } else {
        absSample = maxFloat * data->alpha23;
    }

    if (softclipGain_) {
        absSample *= data->gainCmp;
    }

    return fl2fp(absSample * sign);
}

void AudioOutDriver::clipToMix() {

    // Cache damp as fixed-point — recompute only when volume changes.
    // This avoids per-sample fp2fl/fl2fp round-trips for master volume.
    if (masterVolume_ != cachedVolume_) {
        cachedVolume_ = masterVolume_;
        float v = (float)masterVolume_ / 100.0f;
        cachedDamp_ = v * v * v * v; // x^4 without pow()
        cachedDampFixed_ = fl2fp(cachedDamp_);
    }
    fixed dampFixed = cachedDampFixed_;
    bool interlaced = driver_->Interlaced();

    if (!hasSound_) {
        SYS_MEMSET(mixBuffer_, 0, sampleCount_ * 2 * sizeof(short));
        finalLastPeak_.store(0.0f, std::memory_order_relaxed);
        // Push silence to oscilloscope
        MixerService *ms = MixerService::GetInstance();
        if (ms) {
            float zero = 0.0f;
            ms->PushOscilloscopeSamples(&zero, 1);
        }
    } else {
        short *s1 = mixBuffer_;
		short *s2 = (interlaced) ? s1 + 1 : s1 + sampleCount_;
		int offset = (interlaced) ? 2 : 1;

        fixed *p = primarySoundBuffer_;

        // Oscilloscope: capture during the main loop to avoid a second pass.
        const int maxScope = 128;
        int scopeStep = sampleCount_ / maxScope;
        if (scopeStep < 1) scopeStep = 1;
        float scopeBuf[128];
        int scopeCount = 0;

        // Peak tracking in fixed-point (avoid per-sample float conversion)
        fixed peakFixed = 0;
        bool doSoftClip = (softclip_ != -1);

        for (int i = 0; i < sampleCount_; i++) {

            fixed l = *p++;
            fixed r = *p++;

            // Apply softclip only when enabled
            if (doSoftClip) {
                l = softClip(l);
                r = softClip(r);
            }

            // Apply master damp entirely in fixed-point (no float round-trip)
            l = fp_mul(l, dampFixed);
            r = fp_mul(r, dampFixed);

            // Hard clip
            l = hardClip(l);
            r = hardClip(r);

            // Track peak in fixed-point (abs)
            fixed al = l < 0 ? -l : l;
            fixed ar = r < 0 ? -r : r;
            if (al > peakFixed) peakFixed = al;
            if (ar > peakFixed) peakFixed = ar;

            // Capture oscilloscope sample (fused into main loop)
            if ((i % scopeStep) == 0 && scopeCount < maxScope) {
                // Mix L+R to mono in fixed, then one float conversion
                fixed mono = (l >> 1) + (r >> 1);
                float monoF = fp2fl(mono) / 32768.0f;
                if (monoF > 1.0f) monoF = 1.0f;
                if (monoF < -1.0f) monoF = -1.0f;
                scopeBuf[scopeCount++] = monoF;
            }

            *s1 = short(fp2i(l));
            s1 += offset;
			*s2 = short(fp2i(r));
			s2 += offset;
        };

        // One float conversion for peak at the end instead of per-sample
        float finalPeak = fp2fl(peakFixed) / 32768.0f;
        if (finalPeak > 2.0f) finalPeak = 2.0f;
        finalLastPeak_.store(finalPeak, std::memory_order_relaxed);

        MixerService *ms = MixerService::GetInstance();
        if (ms) ms->PushOscilloscopeSamples(scopeBuf, scopeCount);
    }
} ;

int AudioOutDriver::GetPlayedBufferPercentage() {
	return driver_->GetPlayedBufferPercentage() ;
} ;

float AudioOutDriver::GetFinalPeak() { return finalLastPeak_.load(std::memory_order_relaxed); } ;

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

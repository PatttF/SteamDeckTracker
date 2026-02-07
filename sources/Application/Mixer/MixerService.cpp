#include "MixerService.h"
#include "Application/Audio/DummyAudioOut.h"
#include "Application/Model/Config.h"
#include "Application/Model/Mixer.h"
#include "Application/Model/Project.h"
#include "Services/Audio/Audio.h"
#include "Services/Audio/AudioDriver.h"
#include "Services/Midi/MidiService.h"
#include "Application/Player/PlayerMixer.h"
#include "System/Console/Trace.h"

MixerService::MixerService() : out_(0), sync_(0), isRendering_(false), pregain_(100), masterVolume_(100), waveformPos_(0) {
    mode_ = MSRM_PLAYBACK;
    for (int i=0;i<WAVE_SAMPLES_CONST;i++) waveform_[i]=0.0f;
    for (int i=0;i<SONG_CHANNEL_COUNT;i++) channelReverbSend_[i]=0.0f;
};

MixerService::~MixerService(){};

/*
 * initializes the mixer service, config changes depending if we're in sequencer or render mode
 */
bool MixerService::Init() {
    // create the output depending on rendering mode
    out_ = 0;
	switch (mode_) {
    case MSRM_STEREO:
    case MSRM_STEMS:
        out_ = new DummyAudioOut();
        break;
    default:
        Audio *audio = Audio::GetInstance();
        out_ = audio->GetFirst();
        break;
	}

	for (int i=0;i<MAX_BUS_COUNT;i++) {
		master_.Insert(bus_[i]);
	}
	// Initialize bus volumes from Mixer model
	Mixer *mixer = Mixer::GetInstance();
	if (mixer) {
		for (int i = 0; i < SONG_CHANNEL_COUNT; i++) {
			int vol = mixer->GetChannelVolume(i);
			int bus = mixer->GetBus(i);
			if (bus < 0 || bus >= MAX_BUS_COUNT) continue;
			fixed fvol = fp_mul(i2fp(vol), fl2fp(0.01f));
			bus_[bus].SetVolume(fvol);
		}
	}

	bool result = false;
	if (out_) {
		result = out_->Init();
		if (result) {
			out_->Insert(master_);
		}

        initRendering(mode_);
        out_->AddObserver(*MidiService::GetInstance());
	}

	sync_=SDL_CreateMutex();
	NAssert(sync_);

	if (result) {
		Trace::Debug("MixerService: output initialized");
	} else {
		Trace::Error("MixerService: failed to initialize output");
	}
	return (result);
};

void MixerService::initRendering(MixerServiceRenderMode mode) {
    switch(mode) {
    case MSRM_PLAYBACK:
        break;
    case MSRM_STEREO:
        out_->SetFileRenderer("project:mixdown.wav");
        break;
    case MSRM_STEMS:
        for (int i = 0; i < SONG_CHANNEL_COUNT; i++) {
            char buffer[1024];
            sprintf(buffer, "project:channel%d.wav", i);
            bus_[i].SetFileRenderer(buffer);
        }
        break;
    }
}

void MixerService::Close() {
	if (out_) {
    out_->RemoveObserver(*MidiService::GetInstance());
		out_->Close() ;
		out_->Empty() ;
		master_.Empty() ;

		switch(mode_) {
        case MSRM_STEMS:
        case MSRM_STEREO:
            break;
        default:
            break;
        }
    }
   for (int i=0;i<MAX_BUS_COUNT;i++) {
	   bus_[i].Empty() ;
   }
	out_=0 ;
	SDL_DestroyMutex(sync_) ;
	sync_=0 ;
} ;

void MixerService::SetRenderMode(int mode) {
    mode_ = MixerServiceRenderMode(mode);
}

bool MixerService::IsRendering() { return isRendering_; }

bool MixerService::Start() {
    MidiService::GetInstance()->Start();
    if (out_) {
        out_->AddObserver(*this);
        out_->Start();
     }
	return true ;
} ;

void MixerService::Stop() {
	MidiService::GetInstance()->Stop() ;
     if (out_) {
      out_->Stop() ;
      out_->RemoveObserver(*this) ;
     }
}

MixBus *MixerService::GetMixBus(int i) {
	if (i < 0 || i >= MAX_BUS_COUNT) {
		return &(bus_[0]) ; // Return first bus as fallback
	}
	return &(bus_[i]) ;
} ;

void MixerService::Update(Observable &o,I_ObservableData *d)  {

  AudioDriver::Event *event=(AudioDriver::Event *)d;
  if (event->type_ == AudioDriver::Event::ADET_BUFFERNEEDED)
  {  
    // Push latest final peak (post master damp) into waveform buffer for visualization
    float finalPeak = 0.0f;
    if (out_) {
        finalPeak = out_->GetFinalPeak();
    }
    // Fallback: if driver doesn't implement final peak, use pre-master master peak
    if (finalPeak == 0.0f) finalPeak = master_.GetLastPeak();
    PushWaveSample(finalPeak);

    Lock() ;
    SetChanged() ;
    NotifyObservers() ;

    out_->Trigger();
    Unlock();
  }
}

bool MixerService::Clipped() {
     return (out_) ? out_->Clipped() : false;
} ;

void MixerService::SetPregain(int vol) {
    Mixer *mixer = Mixer::GetInstance();

    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    if (pregain_ == vol) return; // avoid noisy repeated logs/updates
    pregain_ = vol;

    fixed pregainFactor = fp_mul(i2fp(vol), fl2fp(0.01f));

    for (int i = 0; i < SONG_CHANNEL_COUNT; i++) {
        int chPercent = 100;
        if (mixer) chPercent = mixer->GetChannelVolume(i);
        int bus = mixer ? mixer->GetBus(i) : i;
        if (bus < 0 || bus >= MAX_BUS_COUNT) continue;
        fixed chFactor = fp_mul(i2fp(chPercent), fl2fp(0.01f));
        fixed combined = fp_mul(chFactor, pregainFactor);
        bus_[bus].SetVolume(combined);
    }
};

void MixerService::SetSoftclip(int clip, int gain) {
    if (out_) out_->SetSoftclip(clip, gain);
}

void MixerService::SetMasterVolume(int attn) {
    if (attn < 0) attn = 0;
    if (attn > 100) attn = 100;
    masterVolume_ = attn;

    if (out_) out_->SetMasterVolume(attn);
}

int MixerService::GetPlayedBufferPercentage() {
	return out_ ? out_->GetPlayedBufferPercentage() : 0;
}

// Set per-channel volume (0..100). This maps channel -> bus then sets bus volume.
void MixerService::SetChannelVolume(int channel, int percent) {
    if (channel < 0 || channel >= SONG_CHANNEL_COUNT) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    Mixer *m = Mixer::GetInstance();
    int prev = m->GetChannelVolume(channel);
    m->SetChannelVolumeField(channel, percent);

    int bus = m->GetBus(channel);
    if (bus >= 0 && bus < MAX_BUS_COUNT) {
        // Apply pregain_ so volume changes take effect immediately and are combined with pregain.
        fixed chFactor = fp_mul(i2fp(percent), fl2fp(0.01f));
        fixed pregainFactor = fp_mul(i2fp(pregain_), fl2fp(0.01f));
        fixed combined = fp_mul(chFactor, pregainFactor);
        bus_[bus].SetVolume(combined);
    } else {
    }
}

int MixerService::GetChannelVolume(int channel) {
    if (channel < 0 || channel >= SONG_CHANNEL_COUNT) return 0;
    Mixer *m = Mixer::GetInstance();
    return m->GetChannelVolume(channel);
}

void MixerService::SetChannelPan(int channel, int pan) {
    if (channel < 0 || channel >= SONG_CHANNEL_COUNT) return;
    if (pan < -50) pan = -50;
    if (pan > 50) pan = 50;
    Mixer *m = Mixer::GetInstance();
    m->SetChannelPanField(channel, pan);
    // TODO: pan is stored in model but not yet applied to audio path. Need to update PlayerChannel/MixBus or AudioMixer render path to apply per-channel panning.
    Trace::Debug("MixerService: channel panning storage updated; audio panning not yet applied");
}

int MixerService::GetChannelPan(int channel) {
    if (channel < 0 || channel >= SONG_CHANNEL_COUNT) return 0;
    Mixer *m = Mixer::GetInstance();
    return m->GetChannelPan(channel);
}

void MixerService::ToggleChannelMute(int channel) {
    PlayerMixer *pm = PlayerMixer::GetInstance();
    if (!pm) return;
    bool cur = pm->IsChannelMuted(channel);
    pm->SetChannelMute(channel, !cur);
}

void MixerService::ToggleChannelSolo(int channel) {
    Mixer *m = Mixer::GetInstance();
    PlayerMixer *pm = PlayerMixer::GetInstance();
    if (!m || !pm) return;

    bool newSolo = !m->IsChannelSolo(channel);
    if (newSolo) {
        // store previous mutes and mute others
        for (int i = 0; i < SONG_CHANNEL_COUNT; i++) {
            bool isMuted = pm->IsChannelMuted(i);
            m->SetChannelPrevMute(i, isMuted);
            if (i == channel) {
                pm->SetChannelMute(i, false);
            } else {
                pm->SetChannelMute(i, true);
            }
        }
        m->SetChannelSoloField(channel, true);
    } else {
        // restore prior mutes
        for (int i = 0; i < SONG_CHANNEL_COUNT; i++) {
            bool prev = m->GetChannelPrevMute(i);
            pm->SetChannelMute(i, prev);
            m->SetChannelSoloField(i, false);
        }
    }
}

void MixerService::toggleRendering(bool enable) {
    isRendering_ = enable;
    switch (mode_) {
    case MSRM_PLAYBACK:
        initRendering(MSRM_PLAYBACK);
        break;
    case MSRM_STEREO:
        initRendering(MSRM_STEREO);
        out_->EnableRendering(enable);
        break;
    case MSRM_STEMS:
        initRendering(MSRM_STEMS);
        for (int i = 0; i < SONG_CHANNEL_COUNT; i++) {
            bus_[i].EnableRendering(enable);
        };
        break;
    }
}

void MixerService::OnPlayerStart() {
	toggleRendering(true) ;
} ;

void MixerService::OnPlayerStop() {
	toggleRendering(false) ;
} ;

void MixerService::Execute(FourCC id,float value) {
     if (value>0.5) {
        Audio *audio=Audio::GetInstance() ;
        int volume=audio->GetMixerVolume() ;
        switch(id) {
           case TRIG_VOLUME_INCREASE:
                if (volume<100) volume+=1 ;
                break ;
           case TRIG_VOLUME_DECREASE:
                if (volume>0) volume-=1 ;
                break ;                       
        } ;
        audio->SetMixerVolume(volume) ;
     } ;
}

float MixerService::GetMasterPeak() {
    // Prefer final post-damp peak from the output driver so meters reflect audible level
    if (out_) return out_->GetFinalPeak();
    return master_.GetLastPeak();
}

void MixerService::PushWaveSample(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    waveformPos_ = (waveformPos_ + 1) % WAVE_SAMPLES_CONST;
    waveform_[waveformPos_] = v;
}

float MixerService::GetWaveSample(int index) const {
    if (index < 0) return 0.0f;
    if (index >= WAVE_SAMPLES_CONST) return 0.0f;
    int pos = (waveformPos_ + 1 + index) % WAVE_SAMPLES_CONST; // oldest..newest
    return waveform_[pos];
}

int MixerService::GetWaveSampleCount() const {
    return WAVE_SAMPLES_CONST;
}

AudioOut *MixerService::GetAudioOut() {
	return out_ ;
} ;

int MixerService::GetMasterVolume() {
    return masterVolume_;
} ;


void MixerService::Lock() {
	if (sync_) SDL_LockMutex(sync_) ;
}

void MixerService::Unlock() {
	if (sync_) SDL_UnlockMutex(sync_) ;
}
// Reverb control methods
void MixerService::SetReverbSend(int channel, float amount) {
    if (channel < 0 || channel >= SONG_CHANNEL_COUNT) return;
    if (amount < 0.0f) amount = 0.0f;
    if (amount > 1.0f) amount = 1.0f;
    channelReverbSend_[channel] = amount;
}

float MixerService::GetReverbSend(int channel) {
    if (channel < 0 || channel >= SONG_CHANNEL_COUNT) return 0.0f;
    return channelReverbSend_[channel];
}

void MixerService::SetReverbDecay(float decay) {
    reverb_.SetDecay(decay);
}

void MixerService::SetReverbDamping(float damping) {
    reverb_.SetDamping(damping);
}

float MixerService::GetChannelReverbSend(int channel) {
    if (channel < 0 || channel >= SONG_CHANNEL_COUNT) return 0.0f;
    return channelReverbSend_[channel];
}

void MixerService::SetChannelReverbSend(int channel, float send) {
    if (channel < 0 || channel >= SONG_CHANNEL_COUNT) return;
    if (send < 0.0f) send = 0.0f;
    if (send > 1.0f) send = 1.0f;
    channelReverbSend_[channel] = send;
}
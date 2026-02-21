#include "PlayerMixer.h"
#include "Application/Mixer/MixerService.h"
#include "Application/Model/Mixer.h"
#include "Application/Instruments/I_Effect.h"
#include "Application/Utils/char.h"
#include "Application/Utils/fixed.h"
#include "Services/Midi/MidiService.h"
#include "SyncMaster.h"
#include "System/Console/Trace.h"
#include "System/System/System.h"
#include <math.h>
#include <stdlib.h>

// Instrument-level effect slot (same FourCC as InstrumentView.h)
#define IFXS MAKE_FOURCC('I','F','X','S')

PlayerMixer::PlayerMixer() {

	for (int i=0;i<SONG_CHANNEL_COUNT;i++) {
        lastInstrument_[i]=0 ;
	} ;

    for (int i=0;i<SONG_CHANNEL_COUNT;i++) {
        channel_[i]=new PlayerChannel(i) ;
    }
}

bool PlayerMixer::Init(Project *project) {

	MixerService *ms=MixerService::GetInstance() ;
	if (!ms->Init()) {
			return false ;
	}

	AudioMixer *mixer=ms->GetMixBus(STREAM_MIX_BUS) ;
	mixer->Insert(fileStreamer_) ;

	project_=project ;

	// Init states

	for (int i=0;i<SONG_CHANNEL_COUNT;i++) {
        lastInstrument_[i]=0 ;
	} ;

	// Connect all channels to their respective buses at init time
	Mixer *mixerModel = Mixer::GetInstance();
	for (int i=0;i<SONG_CHANNEL_COUNT;i++) {
		channel_[i]->SetMixBus(mixerModel->GetBus(i));
	}

	clipped_=false ;
	return true ;
} ;

void PlayerMixer::Close()  {

	for (int i=0;i<SONG_CHANNEL_COUNT;i++) {
		channel_[i]->Reset() ;
	}


	MixerService *ms=MixerService::GetInstance() ;
	ms->Close() ;

}

bool PlayerMixer::Start() {
	MixerService *ms=MixerService::GetInstance() ;
	ms->AddObserver(*this) ;
	// Initialize master volume from project when starting so the service reflects project settings
	if (project_) {
		ms->SetMasterVolume(project_->GetMasterVolume());
	}

	// Connect all channels to their respective buses before starting
	Mixer *mixer = Mixer::GetInstance();
	for (int i=0;i<SONG_CHANNEL_COUNT;i++) {
		channel_[i]->SetMixBus(mixer->GetBus(i));
        notes_[i]=0xFF ;
    } ;

	// Apply instrument IFXS (Instrument Effect Slot) from any cached
	// `lastInstrument_` so effects that an instrument declares are
	// active immediately when the mixer starts — even before notes
	// are played. This keeps instrument-level effects "hot".
	if (project_) {
		for (int i = 0; i < SONG_CHANNEL_COUNT; ++i) {
			I_Instrument *lin = lastInstrument_[i];
			if (lin && !channel_[i]->HasActiveEffect()) {
				Variable *fxVar = lin->FindVariable(IFXS);
				if (fxVar) {
					int slot = fxVar->GetInt();
					if (slot >= 0 && slot < MAX_EFFECT_COUNT) {
						I_Effect *effect = project_->GetEffect(slot);
						if (effect && !effect->IsEmpty()) {
							channel_[i]->SetEffect(effect, 255);
						} else {
							channel_[i]->ClearEffect();
						}
					} else {
						channel_[i]->ClearEffect();
					}
				}
			}
		}
	}

	return ms->Start() ;
} ;

void PlayerMixer::Stop() {
	MixerService *ms=MixerService::GetInstance() ;
	ms->Stop() ;
	ms->RemoveObserver(*this) ;
} ;

void PlayerMixer::StartChannel(int channel) {
	if (channel < 0 || channel >= SONG_CHANNEL_COUNT) return;
	isChannelPlaying_[channel]=true ;
} ;

void PlayerMixer::StopChannel(int channel) {
	if (channel < 0 || channel >= SONG_CHANNEL_COUNT) return;

    StopInstrument(channel) ;
	isChannelPlaying_[channel]=false ;
} ;


bool PlayerMixer::IsChannelPlaying(int channel) {
	if (channel < 0 || channel >= SONG_CHANNEL_COUNT) return false;
	return isChannelPlaying_[channel] ;
} ;

I_Instrument *PlayerMixer::GetLastInstrument(int channel) {
	if (channel < 0 || channel >= SONG_CHANNEL_COUNT) return nullptr;
	return lastInstrument_[channel] ;
} ;


bool PlayerMixer::Clipped() {
     return clipped_ ;
}

void PlayerMixer::Update(Observable &o,I_ObservableData *d) {

  // Notifies the player so that pattern data is processed
  SetChanged();
  NotifyObservers();

  // Only re-read bus assignments when the mixer model has changed.
  // SetMixBus already early-exits on same value, but we avoid calling
  // Mixer::GetInstance()/GetBus() for all 16 channels on every tick.
  Mixer *mixer = Mixer::GetInstance();

  for (int i=0;i<SONG_CHANNEL_COUNT;i++) {
    int bus = mixer->GetBus(i);
    channel_[i]->SetMixBus(bus);
  }

  MixerService *ms=MixerService::GetInstance();

  // Cache pregain/softclip to avoid redundant calls when values haven't changed
  int pregain = project_->GetPregain();
  if (pregain != cachedPregain_) {
    cachedPregain_ = pregain;
    ms->SetPregain(pregain);
  }

  int softclip = project_->GetSoftclip();
  int softclipGain = project_->GetSoftclipGain();
  if (softclip != cachedSoftclip_ || softclipGain != cachedSoftclipGain_) {
    cachedSoftclip_ = softclip;
    cachedSoftclipGain_ = softclipGain;
    ms->SetSoftclip(softclip, softclipGain);
  }

  // Do not overwrite master volume on every audio update; master is controlled by MixerView and persisted to Project
  clipped_=ms->Clipped();
} ;


void PlayerMixer::ReleaseInstrument(I_Instrument *instrument) {
	if (!instrument) return;
	for (int i = 0; i < SONG_CHANNEL_COUNT; i++) {
		// If this channel is currently rendering the instrument, stop it.
		// StopInstrument acquires PlayerChannel's startStopMutex_ which
		// ensures Render() is not mid-flight on that channel. Immediately
		// force-clear the channel's instrument reference so the audio
		// thread cannot access freed memory after we delete the object.
		if (channel_[i]->GetInstrument() == instrument) {
			channel_[i]->StopInstrument();
			// Force-clear to guarantee the channel no longer references the
			// instrument (bypass release tail if necessary).
			channel_[i]->ClearInstrument();
			notes_[i] = 0xFF;
		}
		// Clear cached lastInstrument_ to prevent dangling pointer reuse
		if (lastInstrument_[i] == instrument) {
			lastInstrument_[i] = 0;
		}
	}
}

void PlayerMixer::ForceClearAllChannels() {
	for (int i = 0; i < SONG_CHANNEL_COUNT; i++) {
		// Stop and then forcibly clear the instrument pointer so audition
		// timers can't get stuck on release tails from third-party plugins.
		channel_[i]->StopInstrument();
		channel_[i]->ClearInstrument();
		channel_[i]->Reset();
		notes_[i] = 0xFF;
		lastInstrument_[i] = nullptr;
	}
}

void PlayerMixer::ReleaseEffect(I_Effect *effect) {
	if (!effect) return;
	for (int i = 0; i < SONG_CHANNEL_COUNT; i++) {
		// ClearEffect acquires PlayerChannel's startStopMutex_ which
		// ensures Render() is not mid-flight on that channel.
		channel_[i]->ClearEffect();
	}
}

void PlayerMixer::StartInstrument(int channel,I_Instrument *instrument,unsigned char note,bool newInstrument)  {
	if (channel < 0 || channel >= SONG_CHANNEL_COUNT) return;
	channel_[channel]->StartInstrument(instrument,note,newInstrument) ;
	lastInstrument_[channel]=instrument ;
	notes_[channel]=note ;

	// Auto-apply the instrument's effect slot (IFXS) to this channel only
	// if the channel does not already have an active effect. This lets
	// FXSN/other commands persist effects across notes until explicitly
	// modified or cleared by another command.
	if (!channel_[channel]->HasActiveEffect()) {
		Variable *fxVar = instrument->FindVariable(IFXS);
		if (fxVar && project_) {
			int slot = fxVar->GetInt();
			if (slot >= 0 && slot < MAX_EFFECT_COUNT) {
				I_Effect *effect = project_->GetEffect(slot);
				if (effect && !effect->IsEmpty()) {
					channel_[channel]->SetEffect(effect, 255);
				} else {
					channel_[channel]->ClearEffect();
				}
			} else {
				channel_[channel]->ClearEffect();
			}
		} else {
			channel_[channel]->ClearEffect();
		}
	}
} ;

void PlayerMixer::StopInstrument(int channel) {
    if (channel < 0 || channel >= SONG_CHANNEL_COUNT) return;
    channel_[channel]->StopInstrument() ;
    notes_[channel]=0xFF ;
}

I_Instrument *PlayerMixer::GetInstrument(int channel) {
    if (channel < 0 || channel >= SONG_CHANNEL_COUNT) return nullptr;
    return channel_[channel]->GetInstrument();
}

int PlayerMixer::GetPlayedBufferPercentage() {
	MixerService *ms=MixerService::GetInstance() ;
	return ms->GetPlayedBufferPercentage() ;
};

void PlayerMixer::SetChannelMute(int channel,bool mode) {
     if (channel < 0 || channel >= SONG_CHANNEL_COUNT) return;
     channel_[channel]->SetMute(mode) ;
}

bool PlayerMixer::IsChannelMuted(int channel) {
     if (channel < 0 || channel >= SONG_CHANNEL_COUNT) return false;
     return channel_[channel]->IsMuted() ;
}

PlayerChannel *PlayerMixer::GetChannel(int channel) {
    if (channel < 0 || channel >= SONG_CHANNEL_COUNT) return nullptr;
    return channel_[channel];
}

void PlayerMixer::StartStreaming(const Path &path) {
	fileStreamer_.Start(path) ;
} ;

void PlayerMixer::StopStreaming() {
	fileStreamer_.Stop() ;
} ;

void PlayerMixer::OnPlayerStart() {
	MixerService *ms=MixerService::GetInstance() ;
	ms->OnPlayerStart();
}

void PlayerMixer::OnPlayerStop() {
	MixerService *ms=MixerService::GetInstance() ;
	ms->OnPlayerStop();
}

static char noteBuffer[5] ;

int PlayerMixer::GetChannelNote(int channel) {
	if (channel < 0 || channel >= SONG_CHANNEL_COUNT) return 0xFF;
	return notes_[channel] ;
}

char *PlayerMixer::GetPlayedNote(int channel) {

    if (notes_[channel]!=0xFF) {
		note2visualizer(notes_[channel],noteBuffer) ; 
		return noteBuffer ;
    }
    return "  " ;
} ;

char *PlayerMixer::GetPlayedOctive(int channel) {
    if (notes_[channel]!=0xFF) {
		if (!IsChannelMuted(channel)) {
	        oct2visualizer(notes_[channel],noteBuffer) ; 
	        return noteBuffer ;
		} else {
			return "--" ;
		}
    }
    return "  " ;
} ;

AudioOut *PlayerMixer::GetAudioOut() {
	MixerService *ms=MixerService::GetInstance() ;
	return ms->GetAudioOut();
} ;

void PlayerMixer::Lock() {
	MixerService *ms=MixerService::GetInstance() ;
	ms->Lock() ;
} ;

void PlayerMixer::Unlock() {
	MixerService *ms=MixerService::GetInstance() ;
	ms->Unlock() ;
};

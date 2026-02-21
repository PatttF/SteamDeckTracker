
#include "PlayerChannel.h"
#include "Application/Player/SyncMaster.h"
#include "Application/Mixer/MixerService.h"
#include "Application/Model/Mixer.h"
#include "Application/Instruments/I_Effect.h"
#include "Application/Instruments/VST3Effect.h"
#include "Application/Instruments/LV2Effect.h"
#include "System/Console/Trace.h"

PlayerChannel::PlayerChannel(int index) {             
    index_=index ;
    instr_=0 ;
    releasing_=false ;
    muted_=false ;
	mixBus_=0 ;
	busIndex_=-1 ;
	activeEffect_=nullptr ;
	effectWetDry_=255 ;
}

PlayerChannel::~PlayerChannel() {
}

void PlayerChannel::StartInstrument(I_Instrument *instr,unsigned char note,bool trigger) {
   SysMutexLocker locker(startStopMutex_);

   startAttempts_.fetch_add(1);

   // Stop any existing instrument (inline to avoid deadlock since we already hold the lock)
   if (instr_) {
      instr_->Stop(index_) ;
      instr_=0 ;
      releasing_=false ;
   }
   if (instr->Start(index_,note,trigger)) { // note could be refused coz it's out of the keymap
	   instr_=instr ;
	   startSuccess_.fetch_add(1);
   } else {
	   instr_=0 ;
	   startFail_.fetch_add(1);
   };
} ;

void PlayerChannel::StopInstrument() {
     SysMutexLocker locker(startStopMutex_);
     if (instr_) {
       instr_->Stop(index_) ;
       releasing_=true ;  // Keep instr_ alive so Render() plays release tail
     }
} ;

bool PlayerChannel::Render(fixed *buffer,int samplecount) {
   I_Instrument *localInstr = 0;
   bool localReleasing = false;
   I_Effect *localEffect = nullptr;
   int localWetDry = 255;
   {
       SysMutexLocker locker(startStopMutex_);
       localInstr = instr_;
       localReleasing = releasing_;
       localEffect = activeEffect_;
       localWetDry = effectWetDry_;
   }
   if (localInstr) {
     bool tableSlice=SyncMaster::GetInstance()->TableSlice() ;
     bool status=localInstr->Render(index_,buffer,samplecount,tableSlice) ;

     // If instrument reports silence during release, clear the channel
     if (!status && localReleasing) {
       SysMutexLocker locker2(startStopMutex_);
       if (releasing_) {
         instr_=0 ;
         releasing_=false ;
       }
       return false ;
     }

     bool result = ((status)&&(!muted_));

     // Apply audio effect if active
     if (result && localEffect && !localEffect->IsEmpty()) {
        localEffect->ProcessAudio(buffer, samplecount, localWetDry);
     }
     
     return result;
   } else {
     return false ;
   }
} ;

I_Instrument *PlayerChannel::GetInstrument() {
   return instr_ ;
} ;

void PlayerChannel::SetMute(bool muted) {
     muted_=muted ;
}

bool PlayerChannel::IsMuted() {
     return muted_ ;
}

void PlayerChannel::SetMixBus(int i) {

	if (i==busIndex_) return ;

	if (mixBus_) {
		mixBus_->Remove(*this) ;
	}
	mixBus_=MixerService::GetInstance()->GetMixBus(i) ;
	busIndex_=i ;
	if (mixBus_) {
		mixBus_->Insert(*this) ;
	}
} ;

void PlayerChannel::Reset() {
	if (mixBus_) {
		mixBus_->Remove(*this) ;
	}
	releasing_=false ;
	muted_=false ;
	busIndex_=-1 ;
	activeEffect_=nullptr ;
	effectWetDry_=255 ;
} ;

// Immediately clear the instrument reference on this channel (bypass release tail).
// Used only for audition/time-to-live forced-stop so UI doesn't hang on a stuck plugin.
void PlayerChannel::ClearInstrument() {
	SysMutexLocker locker(startStopMutex_);
	instr_ = nullptr;
	releasing_ = false;
}


void PlayerChannel::SetEffect(I_Effect *effect, int wetDry) {
    SysMutexLocker locker(startStopMutex_);
    activeEffect_ = effect;
    effectWetDry_ = wetDry;

    // If the assigned effect exposes a wet/dry variable, keep it in sync
    if (activeEffect_) {
        // Prefer plugin's native wet variable (LV2 or VST3)
        Variable *wv = nullptr;
        wv = activeEffect_->FindVariable(VST3FX_WETDRY);
        if (!wv) wv = activeEffect_->FindVariable(LV2FX_WETDRY);
        if (wv) {
            wv->SetInt(wetDry);
        }

        // Also sync host effect 'volume' into the plugin's native params
        Variable *volVar = activeEffect_->FindVariable(VST3FX_VOLUME);
        if (!volVar) volVar = activeEffect_->FindVariable(LV2FX_VOLUME);
        int hostVol = volVar ? volVar->GetInt() : 0;
        activeEffect_->SyncHostVolume(hostVol);
    }
}

void PlayerChannel::ClearEffect() {
    SysMutexLocker locker(startStopMutex_);
    activeEffect_ = nullptr;
    effectWetDry_ = 255;
}

bool PlayerChannel::HasActiveEffect() {
  SysMutexLocker locker(startStopMutex_);
  return activeEffect_ != nullptr;
}
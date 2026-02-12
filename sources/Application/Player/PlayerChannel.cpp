
#include "PlayerChannel.h"
#include "Application/Player/SyncMaster.h"
#include "Application/Mixer/MixerService.h"
#include "Application/Model/Mixer.h"
#include "Application/Instruments/I_Effect.h"

PlayerChannel::PlayerChannel(int index) {             
    index_=index ;
    instr_=0 ;
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
     }
     instr_=0 ;
} ;

bool PlayerChannel::Render(fixed *buffer,int samplecount) {
   I_Instrument *localInstr = 0;
   I_Effect *localEffect = nullptr;
   int localWetDry = 255;
   {
       SysMutexLocker locker(startStopMutex_);
       localInstr = instr_;
       localEffect = activeEffect_;
       localWetDry = effectWetDry_;
   }
   if (localInstr) {
     bool tableSlice=SyncMaster::GetInstance()->TableSlice() ;
     bool status=localInstr->Render(index_,buffer,samplecount,tableSlice) ;
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
	muted_=false ;
	busIndex_=-1 ;
	activeEffect_=nullptr ;
	effectWetDry_=255 ;
} ;

void PlayerChannel::SetEffect(I_Effect *effect, int wetDry) {
    SysMutexLocker locker(startStopMutex_);
    activeEffect_ = effect;
    effectWetDry_ = wetDry;
}

void PlayerChannel::ClearEffect() {
    SysMutexLocker locker(startStopMutex_);
    activeEffect_ = nullptr;
    effectWetDry_ = 255;
}
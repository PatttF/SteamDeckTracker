
#ifndef _PLAYER_CHANNEL_H_
#define _PLAYER_CHANNEL_H_

#include "Services/Audio/AudioModule.h"
#include "Application/Instruments/I_Instrument.h"
#include "Application/Mixer/MixBus.h"
#include "System/Process/SysMutex.h"
#include <atomic>

class PlayerChannel: public AudioModule {
public:
	// Diagnostics counters (start attempts / success / fail)
	int GetStartAttempts() const { return startAttempts_.load(); }
	int GetStartSuccess() const { return startSuccess_.load(); }
	int GetStartFail() const { return startFail_.load(); }
	PlayerChannel(int index) ;
	virtual ~PlayerChannel() ;
	virtual bool Render(fixed *buffer,int samplecount) ;
	void StartInstrument(I_Instrument *instr,unsigned char note,bool cleanStart) ;
	void StopInstrument() ;
	I_Instrument *GetInstrument() ;
	void SetMute(bool muted) ;
	bool IsMuted() ;
	void SetMixBus(int i) ;
	void Reset() ;
private:
	int index_ ;
	I_Instrument *instr_ ;
	bool muted_ ;
	int busIndex_ ;
	MixBus *mixBus_ ;

	// Mutex protecting start/stop/access to instr_
	SysMutex startStopMutex_ ;

	// Diagnostics
	std::atomic<int> startAttempts_{0};
	std::atomic<int> startSuccess_{0};
	std::atomic<int> startFail_{0};
} ;

#endif

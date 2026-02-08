#ifndef _PLAYER_H_
#define _PLAYER_H_
#include "Foundation/T_Singleton.h"
#include "Foundation/Observable.h"
#include "System/Timer/Timer.h"
#include "Application/Views/ViewData.h"
#include "Application/Views/BaseClasses/ViewEvent.h"
#include "PlayerMixer.h"
#include "SyncMaster.h"

enum PlayerEventType {
	PET_START,
	PET_UPDATE,
	PET_STOP
} ;

enum SequencerMode {
     SM_SONG,
     SM_LIVE
} ;

enum QueueingMode {
     QM_NONE,
     QM_CHAINSTART,
     QM_PHRASESTART,
     QM_CHAINSTOP,
	 QM_PHRASESTOP,
	 QM_TICKSTART
} ;

class PlayerEvent: public ViewEvent {
public:
	PlayerEvent(PlayerEventType type,unsigned int tickCount=0) ;
	PlayerEventType GetType() ;
	unsigned int GetTickCount() ;
private:
	PlayerEventType type_ ;
	unsigned int tickCount_ ;
} ;

class Player: public I_Observer,public Observable,public T_Singleton<Player> {
private: // Singleton
	Player() ;
public:


	static Player *GetInstance() ;
	bool Init(Project*,ViewData *) ;
	void Reset() ;
	void Close() ;
	
	
	virtual void Update(Observable &o,I_ObservableData *d) ;

	// basic interface

	void Start(PlayMode mode,bool forceSongMode) ;
	void Stop() ;
	
//	void Toggle(PlayMode mode,bool forceSongMode=false) ;
//	void ChangePlayMode(PlayMode mode) ;
//	PlayMode GetPlayMode() ;
	
	void SetSequencerMode(SequencerMode mode) ;
	SequencerMode GetSequencerMode() ;
	
	void OnStartButton(PlayMode origin,unsigned int from,bool startFromLastPos,unsigned char chainPos) ;
	void OnSongStartButton(unsigned int from,unsigned int to,bool requestStop,bool forceImmediate) ;
	
	bool IsRunning() ;
	bool Clipped() ;
	
	void ProcessCommands() ;
	bool ProcessChannelCommand(int channel,FourCC cmd,ushort param) ;

	void StartStreaming(const Path &path) ;
	void StopStreaming() ;

	// Channel data
	
	bool IsChannelPlaying(int channel) ;
	void SetChannelMute(int channel,bool mute) ;
	bool IsChannelMuted(int channel) ;
	
	// Live queuing

	QueueingMode GetQueueingMode(int i) ;
	unsigned char GetQueuePosition(int i) ;
	unsigned char GetQueueChainPosition(int i) ;
	void QueueChannel(int i,QueueingMode mode,unsigned char position,unsigned char chainpos=0) ;

	char *GetLiveIndicator(int channel) ;
	double GetPlayTime() ;

	char *GetPlayedNote(int channel) ;
	char *GetPlayedOctive(int channel) ;
	char *GetPlayedInstrument(int channel) ;

	// info
	int GetPlayedBufferPercentage() ;

	std::string GetAudioAPI() ;
	std::string GetAudioDevice() ;
	int GetAudioBufferSize() ;
	int GetAudioRequestedBufferSize() ;
	int GetAudioPreBufferCount() ;

	// set a time-to-live for a channel in milliseconds; will stop instrument after that time
	void SetChannelTimeToLiveMs(int channel, unsigned int ms) ;

	// Internal helpers (declared here so definitions in Player.cpp compile cleanly)
	bool isPlayable(int row,int col,int chainPos) ;
	bool findPlayable(uchar *row,int col,uchar  chainPos) ;
	void triggerLiveChains() ;
	void updateSongPos(int pos,int channel,int chainPos,int hop=-1) ;
	void updateChainPos(int pos,int channel,int hop=-1) ;
	void updatePhrasePos(int pos,int channel) ;
	void playCursorPosition(int channel) ;
	int getChannelHop(int channel,int pos) ;
	void moveToNextStep() ;
	void moveToNextPhrase(int channel,int hop=-1) ;
	void moveToNextChain(int channel,int hop=-1) ;

	PlayerMixer *mixer_ ;
	ViewData *viewData_ ;
	Project *project_ ;

	SequencerMode sequencerMode_ ;
	PlayMode mode_ ;
	PlayMode GetPlayMode() { return mode_; }
	bool isRunning_ ;

	unsigned long startClock_ ;    // .Used to time display live queued chains
								   //  for blinking effect
	unsigned long now_ ;
	int lastPercentage_ ;
	int lastBeatCount_ ;
	unsigned char lastSongPos_ ;
	bool firstPlayCycle_ ;
	bool triggerLiveChains_ ;

	double startTime_ ;

	char instrumentOnChannel_[SONG_CHANNEL_COUNT][3];

	// Live queuing system

    unsigned char liveQueuePosition_[SONG_CHANNEL_COUNT] ;
    QueueingMode liveQueueingMode_[SONG_CHANNEL_COUNT] ;
	unsigned char liveQueueChainPosition_[SONG_CHANNEL_COUNT] ;
	unsigned int timeToLive_[SONG_CHANNEL_COUNT] ;
	unsigned int timeToStart_[SONG_CHANNEL_COUNT] ;
	// realtime expiry clock for time-to-live in milliseconds (0 = none)
	unsigned long timeToLiveClock_[SONG_CHANNEL_COUNT] ;
	unsigned char phraseHopCount_[SONG_CHANNEL_COUNT] ; // HOP loop countdown per channel

	bool retrigAllImmediate_ ;
	unsigned char retrigPos_ ;
	
} ;

#endif

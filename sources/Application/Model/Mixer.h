
#ifndef _MIXER_H_
#define _MIXER_H_

#include "Foundation/T_Singleton.h"
#include "Application/Persistency/Persistent.h"

#include "Song.h"
#include "Application/Utils/fixed.h"

class Mixer:public T_Singleton<Mixer>,Persistent {
public:
	Mixer() ;
	~Mixer() ;
	void Clear() ;

	inline int GetBus(int i) {
		if (i < 0 || i >= SONG_CHANNEL_COUNT) return 0;
		return channelBus_[i];
	};

	virtual void SaveContent(TiXmlNode *node) ;
	virtual void RestoreContent(TiXmlElement *element);
private:
	char channelBus_[SONG_CHANNEL_COUNT] ;
	// UI/state fields
	unsigned char channelVolume_[SONG_CHANNEL_COUNT] ; // 0-100
	char channelPan_[SONG_CHANNEL_COUNT] ; // -50..50
	bool channelSolo_[SONG_CHANNEL_COUNT] ;
	bool channelPrevMute_[SONG_CHANNEL_COUNT] ;

public:
	inline int GetChannelVolume(int i) {
		if (i < 0 || i >= SONG_CHANNEL_COUNT) return 100;
		return channelVolume_[i];
	};
	inline void SetChannelVolumeField(int i,int v) {
		if (i < 0 || i >= SONG_CHANNEL_COUNT) return;
		channelVolume_[i]=(unsigned char)v;
	};
	inline int GetChannelPan(int i) {
		if (i < 0 || i >= SONG_CHANNEL_COUNT) return 0;
		return channelPan_[i];
	};
	inline void SetChannelPanField(int i,int p) {
		if (i < 0 || i >= SONG_CHANNEL_COUNT) return;
		channelPan_[i]=(char)p;
	};
	inline bool IsChannelSolo(int i) {
		if (i < 0 || i >= SONG_CHANNEL_COUNT) return false;
		return channelSolo_[i];
	};
	inline void SetChannelSoloField(int i,bool s) {
		if (i < 0 || i >= SONG_CHANNEL_COUNT) return;
		channelSolo_[i]=s;
	};
	inline void SetChannelPrevMute(int i,bool m) {
		if (i < 0 || i >= SONG_CHANNEL_COUNT) return;
		channelPrevMute_[i]=m;
	};
	inline bool GetChannelPrevMute(int i) {
		if (i < 0 || i >= SONG_CHANNEL_COUNT) return false;
		return channelPrevMute_[i];
	};
} ;

#endif

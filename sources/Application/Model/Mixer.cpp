
#include "Mixer.h"

Mixer::Mixer():Persistent("MIXER")  {
	Clear() ;
} ;

Mixer::~Mixer() {
} ;

void Mixer::Clear() {

	for (int i = 0; i < SONG_CHANNEL_COUNT; i++) {
		channelBus_[i] = i;
		channelVolume_[i] = 100;
		channelPan_[i] = 0;
		channelSolo_[i] = false;
		channelPrevMute_[i] = false;
	}
} ;

void Mixer::SaveContent(TiXmlNode *node) {
} ;

 void Mixer::RestoreContent(TiXmlElement *element) {
}

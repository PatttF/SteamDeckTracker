
#include "Mixer.h"
#include "Application/Mixer/MixerService.h"

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
	// Save per-channel mixer settings
	for (int i = 0; i < SONG_CHANNEL_COUNT; i++) {
		TiXmlElement ch("CHANNEL");
		ch.SetAttribute("INDEX", i);
		ch.SetAttribute("BUS", (int)channelBus_[i]);
		ch.SetAttribute("VOLUME", (int)channelVolume_[i]);
		ch.SetAttribute("PAN", (int)channelPan_[i]);
		ch.SetAttribute("SOLO", channelSolo_[i] ? 1 : 0);
		ch.SetAttribute("PREVMUTE", channelPrevMute_[i] ? 1 : 0);
		node->InsertEndChild(ch);
	}
} ;

 void Mixer::RestoreContent(TiXmlElement *element) {
	// Iterate CHANNEL children and restore saved values
	TiXmlElement *child = element->FirstChildElement("CHANNEL");
	while (child) {
		int idx = -1;
		if (!child->Attribute("INDEX", &idx)) {
			child = child->NextSiblingElement("CHANNEL");
			continue;
		}
		if (idx < 0 || idx >= SONG_CHANNEL_COUNT) {
			child = child->NextSiblingElement("CHANNEL");
			continue;
		}
		int bus = 0;
		child->Attribute("BUS", &bus);
		int vol = 100;
		child->Attribute("VOLUME", &vol);
		int pan = 0;
		child->Attribute("PAN", &pan);
		int solo = 0;
		child->Attribute("SOLO", &solo);
		int prev = 0;
		child->Attribute("PREVMUTE", &prev);

		channelBus_[idx] = (char)bus;
		channelVolume_[idx] = (unsigned char)vol;
		channelPan_[idx] = (char)pan;
		channelSolo_[idx] = (solo != 0);
		channelPrevMute_[idx] = (prev != 0);

		// If MixerService is available, apply the channel volume so audio reflects restored settings immediately
		MixerService *ms = MixerService::GetInstance();
		if (ms) {
			ms->SetChannelVolume(idx, vol);
		}

		child = child->NextSiblingElement("CHANNEL");
	}
}

#ifndef _RT_MIDIOUT_H_
#define _RT_MIDIOUT_H_

#include "Services/Midi/MidiOutDevice.h"
#include "Externals/RtMidi/RtMidi.h"

class RTMidiOutDevice: public MidiOutDevice {
public:
	RTMidiOutDevice(int index,const char *name) ;
	virtual ~RTMidiOutDevice() ;
	virtual bool Init() ;
	virtual void Close() ;
	virtual bool Start() ;
	virtual void Stop();
	virtual bool IsRunning() { return running_; }
	virtual void SendMessage(MidiMessage &m);
private:
	RtMidiOut rtMidiOut_ ;
	int index_ ;
	bool running_ ;
} ;
#endif

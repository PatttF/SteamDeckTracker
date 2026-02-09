
#include "RTMidiOutDevice.h"
#include "System/Console/Trace.h"


RTMidiOutDevice::RTMidiOutDevice(int index,const char *name):
	MidiOutDevice(name),
	index_(index),
	running_(false)
{
} ;

RTMidiOutDevice::~RTMidiOutDevice() {
} ;

bool RTMidiOutDevice::Init(){
	return true ;
} ;

void RTMidiOutDevice::Close(){
}  ;

bool RTMidiOutDevice::Start(){
	try {
		rtMidiOut_.openPort( index_ );
		running_=true ;
		return true ;
	} catch (RtError &error) {
		Trace::Log("RTMidiOutDevice", "Failed to open port %d: %s", index_, error.getMessageString());
		return false ;
	}
}  ;

void RTMidiOutDevice::Stop(){
	running_=false ;
	rtMidiOut_.closePort() ;
} ;

void RTMidiOutDevice::SendMessage(MidiMessage &msg)
{
  if (running_)
  {
    std::vector<unsigned char> message;
    message.push_back(msg.status_) ;

    if (msg.data1_ != MidiMessage::UNUSED_BYTE)
    {
      message.push_back(msg.data1_) ;
    }

    if (msg.data2_ != MidiMessage::UNUSED_BYTE)
    {
      message.push_back(msg.data2_) ;
    }
    Trace::Log("MIDI_RAW", "port=%d [%02X %02X %02X] len=%zu",
               index_, msg.status_,
               msg.data1_ != MidiMessage::UNUSED_BYTE ? msg.data1_ : 0,
               msg.data2_ != MidiMessage::UNUSED_BYTE ? msg.data2_ : 0,
               message.size());
    rtMidiOut_.sendMessage( &message );
  }
}

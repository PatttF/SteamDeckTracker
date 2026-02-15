#include "RTMidiService.h"
#include "RTMidiOutDevice.h"
#include "RTMidiInDevice.h"
#include "System/Console/Trace.h"
#include "Application/Model/Config.h"

//static void CALLBACK MidiFlushProc(UINT uiID, UINT uiMsg, DWORD
//                                  dwUser, DWORD dw1, DWORD dw2) {
//
//	W32MidiService *msvc=(W32MidiService *)dwUser;
//	msvc->Flush();
//};

RTMidiService::RTMidiService() {
	const char *delay = Config::GetInstance()->GetValue("MIDIDELAY");
	if (delay) {
		midiDelay_=atoi(delay);
	} else {
		midiDelay_=0;
	}

	// RtMidiIn constructor
	try {
		rtMidiIn_ = new RtMidiIn();
	} catch ( RtError &error ) {
		
		rtMidiIn_=0;
		
	}

	// RtMidiOut constructor
	try {
		rtMidiOut_ = new RtMidiOut();
	} catch ( RtError &error ) {
		
		rtMidiOut_=0;
		
	}
};

RTMidiService::~RTMidiService() {
	delete rtMidiIn_;
	delete rtMidiOut_;
};

/*
 * here we just loop over existing midi out and create a midi device for each of them
 */
void RTMidiService::buildDriverList() {
	// check inputs
  unsigned int nPorts = (rtMidiIn_)?rtMidiIn_->getPortCount():0;
	
	for (uint i=0; i<nPorts; i++) {
		try {
			std::string portName = rtMidiIn_->getPortName(i);
			RTMidiInDevice *in = new RTMidiInDevice(i, portName.c_str());
			
			inList_.Insert(in);
		} catch (RtError &error) {
      
		}
	}

	// check outputs
	nPorts = (rtMidiOut_)?rtMidiOut_->getPortCount():0;
	
	for (uint i=0; i<nPorts; i++ ) {
		try {
			std::string portName = rtMidiOut_->getPortName(i);
			RTMidiOutDevice *out = new RTMidiOutDevice(i,portName.c_str());
			
			Insert(out);
		} catch (RtError &error) {
			
		}
	}
}

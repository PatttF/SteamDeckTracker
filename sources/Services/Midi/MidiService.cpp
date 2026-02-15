#include "MidiService.h"
#include "Application/Player/SyncMaster.h"
#include "System/Console/Trace.h"
#include "System/Timer/Timer.h"
#include "Application/Model/Config.h"
#include "Services/Audio/AudioDriver.h"
#include "System/Console/Trace.h"

#ifdef SendMessage
#undef SendMessage
#endif

MidiService::MidiService()
    : T_SimpleList<MidiOutDevice>(true), inList_(true), device_(0),
      tickToFlush_(0), sendSync_(true) {
#ifndef _FEAT_MIDI_MULTITHREAD
    for (int i=0;i<MIDI_MAX_BUFFERS;i++) {
		queues_[i]=new T_SimpleList<MidiMessage>(true);
	}
#endif
    const char *delay = Config::GetInstance()->GetValue("MIDIDELAY");
    midiDelay_ = delay ? atoi(delay) : 1;

    const char *sendSync = Config::GetInstance()->GetValue("MIDISENDSYNC");
    if (sendSync) {
		sendSync_ = (strcmp(sendSync,"YES")==0);
	}
};

MidiService::~MidiService() {
	Close();
};

bool MidiService::Init() {
	Empty();
  inList_.Empty();
	buildDriverList();
	// Add a merger for the input
	merger_=new MidiInMerger();
	IteratorPtr<MidiInDevice>it(inList_.GetIterator());
	for (it->Begin();!it->IsDone();it->Next()) {
		MidiInDevice &current=it->CurrentItem();
		merger_->Insert(current);
	}

	return true;
};

void MidiService::Close() {
	Stop();
};

I_Iterator<MidiInDevice> *MidiService::GetInIterator() {
	return inList_.GetIterator();
};

int MidiService::GetOutDeviceCount() {
	return Size();
}

const char *MidiService::GetOutDeviceName(int index) {
	if (index < 0 || index >= Size()) return "";
	IteratorPtr<MidiOutDevice> it(GetIterator());
	int i = 0;
	for (it->Begin(); !it->IsDone(); it->Next(), i++) {
		if (i == index) {
			return it->CurrentItem().GetName();
		}
	}
	return "";
}

void MidiService::SendToDevice(MidiMessage &msg, int deviceIndex) {
	if (deviceIndex < 0 || deviceIndex >= Size()) {
		
		return;
	}
	IteratorPtr<MidiOutDevice> it(GetIterator());
	int i = 0;
	for (it->Begin(); !it->IsDone(); it->Next(), i++) {
		if (i == deviceIndex) {
			MidiOutDevice &dev = it->CurrentItem();
			if (!dev.IsRunning()) {
				if (!dev.Init()) return;
				if (!dev.Start()) {
					dev.Close();
					return;
				}
			}
			dev.SendMessage(msg);
			return;
		}
	}
	
}

void MidiService::StopStartedDevices() {
	// Stop all devices that were auto-started by SendToDevice
	IteratorPtr<MidiOutDevice> it(GetIterator());
	for (it->Begin(); !it->IsDone(); it->Next()) {
		MidiOutDevice &dev = it->CurrentItem();
		if (dev.IsRunning()) {
			dev.Stop();
			dev.Close();
		}
	}
}

void MidiService::RefreshDevices() {
	// Stop any running devices before rebuilding list
	StopStartedDevices();
	// Clear project-level device pointer (it points into our list)
	if (device_) {
		device_ = 0;
	}
	// Clear old device list (T_SimpleList owns them, Empty deletes)
	Empty();
	inList_.Empty();
	// Re-query system for current MIDI ports
	rebuildDriverList();
	// Rebuild input merger
	if (merger_) {
		delete merger_;
	}
	merger_ = new MidiInMerger();
	IteratorPtr<MidiInDevice> it(inList_.GetIterator());
	for (it->Begin(); !it->IsDone(); it->Next()) {
		MidiInDevice &current = it->CurrentItem();
		merger_->Insert(current);
	}
	
}

void MidiService::rebuildDriverList() {
	// Default: just call the initial builder
	buildDriverList();
}

void MidiService::SelectDevice(const std::string &name) {
	deviceName_ = name;
};

bool MidiService::Start() {
#ifndef _FEAT_MIDI_MULTITHREAD
    currentPlayQueue_ = 0;
    currentOutQueue_ = 0;
#endif
    return true;
}

void MidiService::Stop() {
	stopDevice();
	StopStartedDevices();
}

#ifdef _FEAT_MIDI_MULTITHREAD
// For multi-threaded systems we use a concurrentqueue
void MidiService::QueueMessage(MidiMessage &m) {
    if (!device_)
        return;
    // 

    midiQueue_.enqueue(m);
}
#else
// For single-threaded systems we do it the old way
void MidiService::QueueMessage(MidiMessage &m) {
    if (!device_)
        return;

    T_SimpleList<MidiMessage> *queue = queues_[currentPlayQueue_];
    MidiMessage *ms = new MidiMessage(m.status_, m.data1_, m.data2_);
    queue->Insert(ms);
}
#endif

void MidiService::Trigger() {
#ifndef _FEAT_MIDI_MULTITHREAD
    AdvancePlayQueue();
#endif
    if (device_ && sendSync_) {
        SyncMaster *sm=SyncMaster::GetInstance();
		if (sm->MidiSlice()) {
            MidiMessage msg;
            msg.status_ = 0xF8;
            QueueMessage(msg);
        }
    }
}

#ifndef _FEAT_MIDI_MULTITHREAD
void MidiService::AdvancePlayQueue() {
    int next = (currentPlayQueue_ + 1) % MIDI_MAX_BUFFERS;
    queues_[next]->Empty();
    currentPlayQueue_ = next;
}
#endif

void MidiService::Update(Observable &o,I_ObservableData *d) {
  AudioDriver::Event *event=(AudioDriver::Event *)d;
  if (event->type_ == AudioDriver::Event::ADET_DRIVERTICK) {
    onAudioTick();
  }
};

void MidiService::onAudioTick() {
    if (tickToFlush_ > 0) {
        if (--tickToFlush_ ==0) {
			flushOutQueue();
        }
    }
}

void MidiService::Flush() {
    tickToFlush_ = midiDelay_;
    if (tickToFlush_ == 0) {
		flushOutQueue();
    }
};

#ifdef _FEAT_MIDI_MULTITHREAD
void MidiService::flushOutQueue() {
    if (!device_)
        return;

    MidiMessage msg;
    T_SimpleList<MidiMessage> batch;
    // Drain all messages currently in the queue
    while (midiQueue_.try_dequeue(msg)) {
        batch.Insert(msg);
        // 
    }
    if (batch.Size() > 0) {
        device_->SendQueue(batch);
		// 
    }
}
#else
void MidiService::flushOutQueue() {
    int next = (currentOutQueue_ + 1) % MIDI_MAX_BUFFERS;
    T_SimpleList<MidiMessage> *flushQueue = queues_[next];
    if (device_) {
        device_->SendQueue(*flushQueue);
    }

    flushQueue->Empty();
    currentOutQueue_ = next;  // Advance only after safe flush
}
#endif

/*
 * starts midi device
 */
void MidiService::startDevice() {
	IteratorPtr<MidiOutDevice>it(GetIterator()) ;

	for (it->Begin(); !it->IsDone(); it->Next()) {
		MidiOutDevice &current = it->CurrentItem();
		if (!strcmp(deviceName_.c_str(), current.GetName())) {
			// Don't re-start if already running (e.g. started by SendToDevice)
			if (current.IsRunning()) {
				device_ = &current;
				
			} else if (current.Init()) {
				if (current.Start()) {
					
					device_ = &current;
				} else {
					
					current.Close();
				}
			}
			break;
		}
	}
};

/*
 * closes midi device
 */
void MidiService::stopDevice() {
	if (device_) {
		if (device_->IsRunning()) {
			device_->Stop() ;
			device_->Close() ;
		}
	}
	device_=0 ;
} ;

/*
 * starts midi device when playback starts
 */
void MidiService::OnPlayerStart() {
	if (deviceName_.size()!=0) {
		stopDevice();
		startDevice();
		deviceName_="";
	} else {
    startDevice();
  }

	if (sendSync_) {
		MidiMessage msg ;
		msg.status_=0xFA ;
		QueueMessage(msg) ;
	}
};

/*
 * queues midi stop message when player stops
 */
void MidiService::OnPlayerStop() {
	if (sendSync_) {
		MidiMessage msg ;
		msg.status_=0xFC ;
		QueueMessage(msg) ;
	}
};

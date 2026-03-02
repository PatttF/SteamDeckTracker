#ifndef _I_INSTRUMENT_H_
#define _I_INSTRUMENT_H_

#include "Foundation/Variables/VariableContainer.h"
#include "Foundation/Observable.h"
#include "Application/Utils/fixed.h"
#include <map>

#include "Application/Player/TablePlayback.h"

enum InstrumentType {
	IT_SAMPLE=0,
	IT_MIDI,
	IT_LV2,
	IT_SOUNDFONT,
	IT_VST3,
	IT_MIDIOUT,
	IT_AUDIOIN,
	IT_LAST
} ;

class I_Instrument:public VariableContainer, public Observable {
      
public:
	I_Instrument() : nextVelocity_(127) {} ;
	virtual ~I_Instrument() {} ;

	  // Initialisation routine

	  virtual bool Init()=0 ;

	  // Start & stop the instument
      virtual bool Start(int channel,unsigned char note,bool retrigger=true)=0 ;
      virtual void Stop(int channel)=0 ;

	  // Engine playback start/stop callbacks

	  virtual void OnStart()=0 ;
	  virtual void OnStop() {}

      // size refers to the number of samples
      // should always fill interleaved stereo / 16bit
      
      virtual bool Render(int channel,fixed *buffer,int size,bool updateTick)=0 ;

      virtual bool IsInitialized()=0 ;

	  virtual bool IsEmpty()=0 ;

	  virtual InstrumentType GetType()=0 ;

	  virtual const char *GetName()=0 ; 
	 
	  virtual void ProcessCommand(int channel,FourCC cc,ushort value)=0 ;

	  virtual void Purge()=0 ;

	  virtual int GetTable()=0 ;
	  virtual bool GetTableAutomation()=0 ;

	  virtual void GetTableState(TableSaveState &state)=0 ;	 
	  virtual void SetTableState(TableSaveState &state)=0 ;	 

	  // Set the velocity for the next Start() call (0-127). Instruments
	  // that support velocity should consume nextVelocity_ in Start().
	  void SetNextVelocity(unsigned char vel) { nextVelocity_ = vel; }

	  // Queue a raw MIDI event (CC, aftertouch, pitch bend, etc.) for
	  // instruments that support it. Default does nothing.
	  virtual void QueueMidiEvent(unsigned char status, unsigned char data1, unsigned char data2) {}

	  // --- Variable-based MIDI CC bindings (Sample/SF2 instruments) ---
	  // Maps a raw CC number to a Variable identified by FourCC ID, with
	  // explicit min/max so the CC 0-127 range maps to the variable's range.

	  struct UserCCVarBinding { FourCC varId; int min; int max; };

	  void SetUserCCVar(int cc, FourCC varId, int min, int max) {
	      // Remove any existing binding for this varId first
	      for (auto it = userCcToVarBinding_.begin(); it != userCcToVarBinding_.end(); )
	          if (it->second.varId == varId) it = userCcToVarBinding_.erase(it); else ++it;
	      userCcToVarBinding_[cc] = {varId, min, max};
	  }

	  void ClearUserCCForVarId(FourCC varId) {
	      for (auto it = userCcToVarBinding_.begin(); it != userCcToVarBinding_.end(); )
	          if (it->second.varId == varId) it = userCcToVarBinding_.erase(it); else ++it;
	  }

	  int GetUserCCForVarId(FourCC varId) const {
	      for (const auto &kv : userCcToVarBinding_)
	          if (kv.second.varId == varId) return kv.first;
	      return -1;
	  }

	  void ApplyUserCCVar(int ccNum, int rawValue) {
	      auto it = userCcToVarBinding_.find(ccNum);
	      if (it != userCcToVarBinding_.end()) {
	          const UserCCVarBinding &b = it->second;
	          Variable *v = FindVariable(b.varId);
	          if (v) {
	              int scaled = b.min + (int)((rawValue & 0x7F) / 127.0 * (b.max - b.min) + 0.5);
	              if (scaled < b.min) scaled = b.min;
	              if (scaled > b.max) scaled = b.max;
	              v->SetInt(scaled, false);
	          }
	      }
	  }

	  const std::map<int, UserCCVarBinding> &GetUserCcVarMap() const { return userCcToVarBinding_; }

protected:
	  unsigned char nextVelocity_;
	  std::map<int, UserCCVarBinding> userCcToVarBinding_;
};
#endif

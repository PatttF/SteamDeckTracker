#ifndef _SRP_UPDATERS_H_
#define _SRP_UPDATERS_H_

#include "I_SRPUpdater.h"
#include "Foundation/Types/Types.h"


class VolumeRamp: public I_SRPUpdater {
public:
	VolumeRamp() {} ;
	virtual ~VolumeRamp() {} ;
	void SetData(float target,float speed,float start) ;
	virtual void Trigger(bool tableTick) ;
	virtual void UpdateSRP(struct RUParams &rup) ;
private:
	fixed current_ ;
	fixed target_ ;
	fixed speed_ ;
} ;

class FCRamp: public I_SRPUpdater {
public:
	FCRamp() {} ;
	virtual ~FCRamp() {} ;
	void SetData(float target,float speed,float start) ;
	virtual void Trigger(bool tableTick) ;
	virtual void UpdateSRP(struct RUParams &rup) ;
private:
	fixed current_ ;
	fixed target_ ;
	fixed speed_ ;
} ;

class FRRamp: public I_SRPUpdater {
public:
	FRRamp() {} ;
	virtual ~FRRamp() {} ;
	void SetData(float target,float speed,float start) ;
	virtual void Trigger(bool tableTick) ;
	virtual void UpdateSRP(struct RUParams &rup) ;
private:
	fixed current_ ;
	fixed target_ ;
	fixed speed_ ;
} ;

class FBMixRamp: public I_SRPUpdater {
public:
	FBMixRamp() {} ;
	virtual ~FBMixRamp() {} ;
	void SetData(float target,float speed,float start) ;
	virtual void Trigger(bool tableTick) ;
	virtual void UpdateSRP(struct RUParams &rup) ;
private:
	fixed current_ ;
	fixed target_ ;
	fixed speed_ ;
} ;

class FBTunRamp: public I_SRPUpdater {
public:
	FBTunRamp() {} ;
	virtual ~FBTunRamp() {} ;
	void SetData(float target,float speed,float start) ;
	virtual void Trigger(bool tableTick) ;
	virtual void UpdateSRP(struct RUParams &rup) ;
private:
	fixed current_ ;
	fixed target_ ;
	fixed speed_ ;
} ;

class LogSpeedRamp: public I_SRPUpdater {
public:
	LogSpeedRamp() {} ;
	virtual ~LogSpeedRamp() {} ;
	void SetData(float target,float speed,float start) ;
	float GetCurrent() ;
	virtual void Trigger(bool tableTick) ;
	virtual void UpdateSRP(struct RUParams &rup) ;
private:
	fixed current_ ;
	fixed target_ ;
	fixed speed_ ;
} ;

class LinSpeedRamp: public I_SRPUpdater {
public:
	LinSpeedRamp() {} ;
	virtual ~LinSpeedRamp() {} ;
	void SetData(float target,float speed,float start) ;
	virtual void Trigger(bool tableTick) ;
	virtual void UpdateSRP(struct RUParams &rup) ;
private:
	fixed current_ ;
	fixed target_ ;
	fixed speed_ ;
} ;

class Arp: public I_SRPUpdater {
public:
	Arp() {} ;
	virtual ~Arp() {} ;
	void SetData(uint data) ;
	virtual void Trigger(bool tableTick) ;
	virtual void UpdateSRP(struct RUParams &rup) ;
private:
	uchar arp_[5] ;     // Arp setting
	uchar arpPosition_ ;// Position of in the arpegiator
	uchar arpLength_ ;  // Length of arp data
	fixed current_  ;
} ;

class Panner: public I_SRPUpdater {
public:
	Panner() {} ;
	virtual ~Panner() {} ;
	void SetData(float target,float speed,float start) ;
	virtual void Trigger(bool tableTick) ;
	virtual void UpdateSRP(struct RUParams &rup) ;
private:
	fixed current_ ;
	fixed target_ ;
	fixed speed_ ;
} ;


class Vibrato: public I_SRPUpdater {
public:
	Vibrato() : phase_(0), speed_(0), depth_(0) {} ;
	virtual ~Vibrato() {} ;
	void SetData(unsigned char speed, unsigned char depth) ;
	virtual void Trigger(bool tableTick) ;
	virtual void UpdateSRP(struct RUParams &rup) ;
private:
	fixed phase_ ;   // Current LFO phase (fixed point, wraps at 2*PI)
	fixed speed_ ;   // Phase increment per k-rate tick
	fixed depth_ ;   // Depth of pitch modulation
} ;

class Tremolo: public I_SRPUpdater {
public:
	Tremolo() : phase_(0), speed_(0), depth_(0) {} ;
	virtual ~Tremolo() {} ;
	void SetData(unsigned char speed, unsigned char depth) ;
	virtual void Trigger(bool tableTick) ;
	virtual void UpdateSRP(struct RUParams &rup) ;
private:
	fixed phase_ ;   // Current LFO phase (fixed point, wraps at 2*PI)
	fixed speed_ ;   // Phase increment per k-rate tick
	fixed depth_ ;   // Depth of volume modulation
} ;

class LFOFilter: public I_SRPUpdater {
public:
	LFOFilter() : phase_(0), speed_(0), depth_(0) {} ;
	virtual ~LFOFilter() {} ;
	void SetData(unsigned char speed, unsigned char depth) ;
	virtual void Trigger(bool tableTick) ;
	virtual void UpdateSRP(struct RUParams &rup) ;
private:
	fixed phase_ ;   // Current LFO phase (fixed point, wraps at 256)
	fixed speed_ ;   // Phase increment per k-rate tick
	fixed depth_ ;   // Depth of filter cutoff modulation
} ;

#endif


#include "SRPUpdaters.h"
#include <math.h>
#include "System/Console/Trace.h"
//
// Volume Ramp
//

void VolumeRamp::SetData(float target,float speed,float start) {
	target_=fl2fp(target) ;
	speed_=(speed==0)?0:fl2fp(speed) ;
	current_=fl2fp(start) ;
} ;

void VolumeRamp::Trigger(bool tableTick) {
	if (!enabled_) return ;
	if (!tableTick) {
		if (speed_==0) {
			current_=target_ ;
		} else {
			if (current_<target_) {
				current_=fp_add(current_,speed_) ;
				if (current_>target_) {
					current_=target_ ;
				}
			} else {
				current_=fp_sub(current_,speed_) ;
				if (current_<target_) {
					current_=target_ ;
				}
			} ;
		}
	} ;
};

void VolumeRamp::UpdateSRP(struct RUParams &rup) {
	if (!enabled_) return ;
	rup.volumeOffset_=fp_add(rup.volumeOffset_,current_) ;
} ;

//
// FilterCut off ramp
//

void FCRamp::SetData(float target,float speed,float start) {
	target_=fl2fp(target) ;
	speed_=(speed==0)?0:fl2fp(speed) ;
	current_=fl2fp(start) ;
} ;

void FCRamp::Trigger(bool tableTick) {
	if (!enabled_) return ;
	if (!tableTick) {
		if (speed_==0) {
			current_=target_ ;
		} else {
			if (current_<target_) {
				current_=fp_add(current_,speed_) ;
				if (current_>target_) {
					current_=target_ ;
				}
			} else {
				current_=fp_sub(current_,speed_) ;
				if (current_<target_) {
					current_=target_ ;
				}
			} ;
		}
	} ;
};

void FCRamp::UpdateSRP(struct RUParams &rup) {
	if (!enabled_) return ;
	rup.cutOffset_=fp_add(rup.cutOffset_,current_) ;
} ;

//
// Filter Resonance ramp
//

void FRRamp::SetData(float target,float speed,float start) {
	target_=fl2fp(target) ;
	speed_=(speed==0)?0:fl2fp(speed) ;
	current_=fl2fp(start) ;
} ;

void FRRamp::Trigger(bool tableTick) {
	if (!enabled_) return ;
	if (!tableTick) {
		if (speed_==0) {
			current_=target_ ;
		} else {
			if (current_<target_) {
				current_=fp_add(current_,speed_) ;
				if (current_>target_) {
					current_=target_ ;
				}
			} else {
				current_=fp_sub(current_,speed_) ;
				if (current_<target_) {
					current_=target_ ;
				}
			} ;
		}
	} ;
};

void FRRamp::UpdateSRP(struct RUParams &rup) {
	if (!enabled_) return ;
	rup.resOffset_=fp_add(rup.resOffset_,current_) ;
} ;

// Feedmack mix ramp

void FBMixRamp::SetData(float target,float speed,float start) {
	target_=fl2fp(target) ;
	speed_=(speed==0)?0:fl2fp(speed) ;
	current_=fl2fp(start) ;
} ;

void FBMixRamp::Trigger(bool tableTick) {
	if (!enabled_) return ;
	if (!tableTick) {
		if (speed_==0) {
			current_=target_ ;
		} else {
			if (current_<target_) {
				current_=fp_add(current_,speed_) ;
				if (current_>target_) {
					current_=target_ ;
				}
			} else {
				current_=fp_sub(current_,speed_) ;
				if (current_<target_) {
					current_=target_ ;
				}
			} ;
		}
	} ;
};

void FBMixRamp::UpdateSRP(struct RUParams &rup) {
	if (!enabled_) return ;
	rup.fbMixOffset_=fp_add(rup.fbMixOffset_,current_) ;
} ;

// Feedmack tune ramp

void FBTunRamp::SetData(float target,float speed,float start) {
	target_=fl2fp(target) ;
	speed_=(speed==0)?0:fl2fp(speed) ;
	current_=fl2fp(start) ;
} ;

void FBTunRamp::Trigger(bool tableTick) {
	if (!enabled_) return ;
	if (!tableTick) {
		if (speed_==0) {
			current_=target_ ;
		} else {
			if (current_<target_) {
				current_=fp_add(current_,speed_) ;
				if (current_>target_) {
					current_=target_ ;
				}
			} else {
				current_=fp_sub(current_,speed_) ;
				if (current_<target_) {
					current_=target_ ;
				}
			} ;
		}
	} ;
};

void FBTunRamp::UpdateSRP(struct RUParams &rup) {
	if (!enabled_) return ;
	rup.fbTunOffset_=fp_add(rup.fbTunOffset_,current_) ;
} ;

//
// Speed/Frequency Ramp
//

void LogSpeedRamp::SetData(float target,float speed,float start) {
	target_=fl2fp(target) ;
	current_=fl2fp(start) ;
	if (target_>current_) {
		speed_=fl2fp(speed) ;
	} else {
		if (speed==0) {
			speed_=0 ;
		} else {
			speed_=fl2fp(1.0F/speed) ;
		}
	}
} ;

float LogSpeedRamp::GetCurrent() {
	return fp2fl(current_) ;
} ;

void LogSpeedRamp::Trigger(bool tableTick) {
	if (!enabled_) return ;
	if (!tableTick) {
		if (speed_==0) {
			current_=target_ ;
		} else {
//			if (current_<target_) {
			if (speed_>FP_ONE) {
				current_=fp_mul(current_,speed_) ;
				if (current_>target_) {
					current_=target_ ;
					speed_=0 ;
				}
			} else {
				if (current_>target_) {
					current_=fp_mul(current_,speed_) ;
					if (current_<target_) {
						current_=target_ ;
					}
				}
			} ;
		}
	} ;
};

void LogSpeedRamp::UpdateSRP(struct RUParams &rup) {
	if (!enabled_) return ;
	rup.speedOffset_=fp_mul(rup.speedOffset_,current_) ;
//	Trace::Debug("Log: current=%f,offset now=%f",fp2fl(current_),fp2fl(rup.speedOffset_)) ;
} ;

//
// Linear Speed/Frequency Ramp
//

void LinSpeedRamp::SetData(float target,float speed,float start) {
	target_=fl2fp(target) ;
	current_=fl2fp(start) ;

	speed_=(speed==0)?0:fl2fp(speed);  
} ;

void LinSpeedRamp::Trigger(bool tableTick) {
	if (!enabled_) return ;
	if (!tableTick) {
		if (speed_==0) {
			current_=target_ ;
		} else {
			if (current_<target_) {
				current_=fp_add(current_,speed_) ;
				if (current_>target_) {
					current_=target_ ;
				}
			} else {
				if (current_>target_) {
					current_=fp_sub(current_,speed_) ;
					if (current_<target_) {
						current_=target_ ;
					}
				}
			} ;
		}
	} ;
};

void LinSpeedRamp::UpdateSRP(struct RUParams &rup) {
	if (!enabled_) return ;
	rup.speedOffset_=fp_mul(rup.speedOffset_,current_) ;
} ;


//
// Panner
//

void Panner::SetData(float target,float speed,float start) {
	target_=fl2fp(target) ;
	current_=fl2fp(start) ;

	speed_=(speed==0)?0:fl2fp(speed);  
} ;

void Panner::Trigger(bool tableTick) {
	if (!enabled_) return ;
	if (!tableTick) {
		if (speed_==0) {
			current_=target_ ;
		} else {
			if (current_<target_) {
				current_=fp_add(current_,speed_) ;
				if (current_>target_) {
					current_=target_ ;
				}
			} else {
				if (current_>target_) {
					current_=fp_sub(current_,speed_) ;
					if (current_<target_) {
						current_=target_ ;
					}
				}
			} ;
		}
	} ;
};

void Panner::UpdateSRP(struct RUParams &rup) {
	if (!enabled_) return ;
	rup.panOffset_=fp_add(rup.panOffset_,current_) ;
} ;

//
// Arpegiator
//

void Arp::SetData(unsigned int value) {
	int arp=value ;
	int position=0 ;
	arp_[0]=0;
	for (int i=0;i<4;i++) {
		arp_[4-i]=(arp&0xF) ;
		if ((arp_[4-i]!=0)&&(position==0)) {
			position=5-i ;
		}
		arp=(arp&0xFFF0)>>4 ;
	}
	arpLength_=position ;
	arpPosition_=0 ;
	current_=fl2fp(1.0) ;
} ;

void Arp::Trigger(bool tableTick) {

	if((!tableTick)||(!enabled_)) return ;
	 if (arpLength_>0) {
		arpPosition_++ ;
		if (arpPosition_>arpLength_) {
			arpPosition_=0 ;
		} ;
		current_=fl2fp(float(pow(2.0,arp_[arpPosition_]/12.0))) ;
	}
} ;

void Arp::UpdateSRP(struct RUParams &rup) {
	if (!enabled_) return ;
	rup.speedOffset_=fp_mul(rup.speedOffset_,current_) ;
} ;

//
// Vibrato - periodic pitch oscillation
//

void Vibrato::SetData(unsigned char speed, unsigned char depth) {
	speed_ = fl2fp(speed * 2.0f) ;   // phase increment per k-rate tick
	depth_ = fl2fp(depth / 255.0f) ; // max deviation (0..1)
	// Don't reset phase_ so vibrato continues smoothly when re-triggered
} ;

void Vibrato::Trigger(bool tableTick) {
	if (!enabled_) return ;
	if (!tableTick) {
		phase_ = fp_add(phase_, speed_) ;
		// Wrap phase at 256 (one full cycle)
		while (phase_ >= i2fp(256)) {
			phase_ = fp_sub(phase_, i2fp(256)) ;
		}
	}
} ;

void Vibrato::UpdateSRP(struct RUParams &rup) {
	if (!enabled_) return ;
	// Compute sine from phase (0..255 maps to 0..2*PI)
	float phaseF = fp2fl(phase_) ;
	float sineVal = sinf(phaseF * 2.0f * 3.14159265f / 256.0f) ;
	// Convert depth to pitch multiplier: depth of 1.0 = 4 semitones
	float depthF = fp2fl(depth_) ;
	float pitchMul = powf(2.0f, sineVal * depthF * 4.0f / 12.0f) ;
	rup.speedOffset_ = fp_mul(rup.speedOffset_, fl2fp(pitchMul)) ;
} ;

//
// Tremolo - periodic volume oscillation
//

void Tremolo::SetData(unsigned char speed, unsigned char depth) {
	speed_ = fl2fp(speed * 2.0f) ;   // phase increment per k-rate tick (faster)
	depth_ = fl2fp(depth / 255.0f) ; // max volume deviation (0..1)
} ;

void Tremolo::Trigger(bool tableTick) {
	if (!enabled_) return ;
	if (!tableTick) {
		phase_ = fp_add(phase_, speed_) ;
		while (phase_ >= i2fp(256)) {
			phase_ = fp_sub(phase_, i2fp(256)) ;
		}
	}
} ;

void Tremolo::UpdateSRP(struct RUParams &rup) {
	if (!enabled_) return ;
	float phaseF = fp2fl(phase_) ;
	float sineVal = sinf(phaseF * 2.0f * 3.14159265f / 256.0f) ;
	float depthF = fp2fl(depth_) ;
	// Modulate volume: sineVal -1..1, depth 1.0 = full mute at trough
	// Scale to volume units (0-255 range matches baseVolume scale)
	float volModF = sineVal * depthF * 255.0f ;
	// Clamp to prevent fixed-point overflow in downstream fp_mul chains
	if (volModF > 255.0f) volModF = 255.0f ;
	if (volModF < -255.0f) volModF = -255.0f ;
	fixed volMod = fl2fp(volModF) ;
	rup.volumeOffset_ = fp_add(rup.volumeOffset_, volMod) ;
} ;

//
// LFO Filter - periodic filter cutoff oscillation
//

void LFOFilter::SetData(unsigned char speed, unsigned char depth) {
	speed_ = fl2fp(speed * 2.0f) ;   // phase increment per k-rate tick
	depth_ = fl2fp(depth / 255.0f) ; // modulation depth (0..1 of cutoff range)
} ;

void LFOFilter::Trigger(bool tableTick) {
	if (!enabled_) return ;
	if (!tableTick) {
		phase_ = fp_add(phase_, speed_) ;
		while (phase_ >= i2fp(256)) {
			phase_ = fp_sub(phase_, i2fp(256)) ;
		}
	}
} ;

void LFOFilter::UpdateSRP(struct RUParams &rup) {
	if (!enabled_) return ;
	float phaseF = fp2fl(phase_) ;
	float sineVal = sinf(phaseF * 2.0f * 3.14159265f / 256.0f) ;
	float depthF = fp2fl(depth_) ;
	// Modulate filter cutoff: depth scales how much the cutoff sweeps
	// Full depth (1.0) sweeps the entire cutoff range
	fixed cutMod = fl2fp(sineVal * depthF) ; // ±1.0 of cutoff range at full depth
	rup.cutOffset_ = fp_add(rup.cutOffset_, cutMod) ;
} ;


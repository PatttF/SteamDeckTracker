#ifndef _REVERB_H_
#define _REVERB_H_

#include "Application/Utils/fixed.h"
#include "Application/Model/Song.h" // For SONG_CHANNEL_COUNT

// Simple stereo delay-based reverb
// Uses a circular buffer with multiple taps at different delay times
// Much simpler than Freeverb but more stable

#define REVERB_BUFFER_SIZE 19200  // 200ms at up to 96kHz

class Reverb {
public:
    Reverb();
    ~Reverb();
    
    void Init();
    void Clear();
    
    // Feed audio into the reverb (called per-channel with send amount)
    void Feed(fixed left, fixed right, float sendAmount);
    
    // Get the mixed reverb output (call once per sample after all channels fed)
    void GetOutput(fixed &left, fixed &right);
    
    // Advance to next sample position
    void Advance();
    
    // Parameters (0.0 to 1.0)
    void SetDecay(float value);
    void SetDamping(float value);
    
    float GetDecay() const { return decay_; }
    float GetDamping() const { return damping_; }
    
private:
    // Circular buffer for delay lines (stereo interleaved)
    fixed buffer_[REVERB_BUFFER_SIZE * 2];
    int writePos_;
    
    // Tap positions (as offsets from write position)
    static const int NUM_TAPS = 4;
    int tapOffsets_[NUM_TAPS];
    float tapGains_[NUM_TAPS];
    fixed tapGainsFixed_[NUM_TAPS]; // Cached fixed-point tap gains
    
    // Parameters
    float decay_;    // Feedback amount (0.0 to 0.95)
    float damping_;  // High frequency damping
    
    // Cached fixed-point parameters (avoid per-sample fl2fp)
    fixed decayFixed_;
    fixed dampCoefFixed_;  // damping_
    fixed dampInvFixed_;   // 1.0 - damping_
    
    // Damping filter state
    fixed dampL_;
    fixed dampR_;
    
    // Accumulated input for this sample
    fixed inputL_;
    fixed inputR_;
    
    bool initialized_;
};

#endif // _REVERB_H_

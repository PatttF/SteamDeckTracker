#include "Reverb.h"
#include <string.h>

Reverb::Reverb() 
    : writePos_(0)
    , decay_(0.5f)
    , damping_(0.3f)
    , dampL_(0)
    , dampR_(0)
    , inputL_(0)
    , inputR_(0)
    , initialized_(false) 
{
    // Initialize tap offsets (in samples) - prime-ish numbers for less metallic sound
    // These create a simple multi-tap delay reverb
    tapOffsets_[0] = 1557;   // ~35ms
    tapOffsets_[1] = 2801;   // ~63ms  
    tapOffsets_[2] = 4409;   // ~100ms
    tapOffsets_[3] = 7127;   // ~161ms
    
    // Tap gains (decreasing for later reflections)
    tapGains_[0] = 0.4f;
    tapGains_[1] = 0.3f;
    tapGains_[2] = 0.2f;
    tapGains_[3] = 0.15f;
}

Reverb::~Reverb() {
}

void Reverb::Init() {
    if (initialized_) return;
    Clear();
    initialized_ = true;
}

void Reverb::Clear() {
    memset(buffer_, 0, sizeof(buffer_));
    writePos_ = 0;
    dampL_ = 0;
    dampR_ = 0;
    inputL_ = 0;
    inputR_ = 0;
}

void Reverb::Feed(fixed left, fixed right, float sendAmount) {
    if (!initialized_) {
        Init();
    }
    
    if (sendAmount < 0.001f) return;
    
    // Accumulate input scaled by send amount
    fixed sendFixed = fl2fp(sendAmount);
    inputL_ = fp_add(inputL_, fp_mul(left, sendFixed));
    inputR_ = fp_add(inputR_, fp_mul(right, sendFixed));
}

void Reverb::GetOutput(fixed &left, fixed &right) {
    if (!initialized_) {
        left = 0;
        right = 0;
        return;
    }
    
    // Read from multiple tap positions and sum
    fixed outL = 0;
    fixed outR = 0;
    
    for (int t = 0; t < NUM_TAPS; t++) {
        int readPos = writePos_ - tapOffsets_[t];
        if (readPos < 0) readPos += REVERB_BUFFER_SIZE;
        
        fixed tapL = buffer_[readPos * 2];
        fixed tapR = buffer_[readPos * 2 + 1];
        
        fixed gain = fl2fp(tapGains_[t]);
        outL = fp_add(outL, fp_mul(tapL, gain));
        outR = fp_add(outR, fp_mul(tapR, gain));
    }
    
    left = outL;
    right = outR;
}

void Reverb::Advance() {
    if (!initialized_) return;
    
    // Read the oldest sample for feedback
    int feedbackPos = writePos_ - (REVERB_BUFFER_SIZE - 1);
    if (feedbackPos < 0) feedbackPos += REVERB_BUFFER_SIZE;
    
    fixed fbL = buffer_[feedbackPos * 2];
    fixed fbR = buffer_[feedbackPos * 2 + 1];
    
    // Apply damping (simple lowpass filter on feedback)
    fixed dampCoef = fl2fp(damping_);
    fixed dampInv = fl2fp(1.0f - damping_);
    dampL_ = fp_add(fp_mul(fbL, dampInv), fp_mul(dampL_, dampCoef));
    dampR_ = fp_add(fp_mul(fbR, dampInv), fp_mul(dampR_, dampCoef));
    
    // Mix input with decayed feedback
    fixed decayFixed = fl2fp(decay_);
    fixed writeL = fp_add(inputL_, fp_mul(dampL_, decayFixed));
    fixed writeR = fp_add(inputR_, fp_mul(dampR_, decayFixed));
    
    // Soft limit to prevent runaway
    const fixed limit = fl2fp(0.95f);
    const fixed neglimit = fl2fp(-0.95f);
    if (writeL > limit) writeL = limit;
    if (writeL < neglimit) writeL = neglimit;
    if (writeR > limit) writeR = limit;
    if (writeR < neglimit) writeR = neglimit;
    
    // Write to buffer
    buffer_[writePos_ * 2] = writeL;
    buffer_[writePos_ * 2 + 1] = writeR;
    
    // Advance write position
    writePos_++;
    if (writePos_ >= REVERB_BUFFER_SIZE) {
        writePos_ = 0;
    }
    
    // Clear accumulated input for next sample
    inputL_ = 0;
    inputR_ = 0;
}

void Reverb::SetDecay(float value) {
    if (value < 0.0f) value = 0.0f;
    if (value > 0.95f) value = 0.95f;  // Limit to prevent infinite feedback
    decay_ = value;
}

void Reverb::SetDamping(float value) {
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    damping_ = value;
}

#ifndef _I_EFFECT_H_
#define _I_EFFECT_H_

#include "Foundation/Variables/VariableContainer.h"
#include "Foundation/Observable.h"
#include "Application/Utils/fixed.h"
#include <string>

class TiXmlNode;
class TiXmlElement;

// Effect type enum
enum EffectType {
    ET_LV2 = 0,
    ET_VST3,
    ET_LAST
};

// Maximum effect slot count
#define MAX_EFFECT_COUNT 0x10

// Generic effect parameter info (read-only view for UI)
struct EffectParameter {
    std::string name;
    std::string groupName;
    float minValue;
    float maxValue;
    float currentValue;
    Variable *variable;     // Variable for UI binding (0–127 scaled)

    // Scale point support (LV2-style discrete labels)
    struct ScalePoint {
        float value;
        std::string label;
    };
    std::vector<ScalePoint> scalePoints;
};

// Abstract base class for audio effects (LV2 and VST3)
class I_Effect : public VariableContainer, public I_Observer {
public:
    virtual ~I_Effect() {}

    // Effect type identification
    virtual EffectType GetEffectType() const = 0;

    // Lifecycle
    virtual bool Init() = 0;
    virtual void Purge() = 0;

    // State queries
    virtual bool IsEmpty() const = 0;
    virtual const char *GetName() const = 0;

    // Plugin management
    virtual void SetForcedOutputChannels(int count) = 0;

    // Audio processing: process interleaved stereo fixed-point buffer in-place
    // Returns true if audio was modified
    virtual bool ProcessAudio(fixed *buffer, int sampleCount, int wetDry = 255) = 0;

    // Sync a host-provided per-effect volume into the plugin's parameters
    // hostVol = 0..255 (same scale as effect wrapper 'volume' variable)
    virtual void SyncHostVolume(int hostVol) { /* no-op default */ }

    // Parameter access (generic)
    virtual int GetParameterCount() const = 0;
    virtual const EffectParameter* GetEffectParameter(int index) const = 0;

    // Scale point label lookup
    virtual std::string GetParameterScalePointLabel(int paramIndex, float value) const = 0;

    // Persistence
    virtual void SaveContent(TiXmlNode *node) = 0;
    virtual void RestoreContent(TiXmlElement *element) = 0;
};

#endif

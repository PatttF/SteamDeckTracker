#ifndef _EFFECT_BANK_H_
#define _EFFECT_BANK_H_

#include "Application/Persistency/Persistent.h"
#include "Application/Instruments/I_Effect.h"
#include "Application/Instruments/LV2Effect.h"

class EffectBank : public Persistent {
public:
    EffectBank();
    ~EffectBank();

    I_Effect *GetEffect(int i);

    // Change the effect type for a given slot (creates new effect, deletes old)
    void SetEffectType(int slot, EffectType type);
    EffectType GetEffectType(int slot) const;

    virtual void SaveContent(TiXmlNode *node);
    virtual void RestoreContent(TiXmlElement *element);

    void Init();

private:
    I_Effect *effects_[MAX_EFFECT_COUNT];
};

#endif

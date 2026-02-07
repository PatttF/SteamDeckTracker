#ifndef _EFFECT_BANK_H_
#define _EFFECT_BANK_H_

#include "Application/Persistency/Persistent.h"
#include "Application/Instruments/LV2Effect.h"

class EffectBank : public Persistent {
public:
    EffectBank();
    ~EffectBank();

    LV2Effect *GetEffect(int i);

    virtual void SaveContent(TiXmlNode *node);
    virtual void RestoreContent(TiXmlElement *element);

    void Init();

private:
    LV2Effect *effects_[MAX_LV2EFFECT_COUNT];
};

#endif

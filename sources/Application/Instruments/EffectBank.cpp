#include "EffectBank.h"
#include "Application/Instruments/VST3Effect.h"
#include "Application/Player/PlayerMixer.h"
#include "Application/Utils/char.h"
#include "System/Console/Trace.h"

EffectBank::EffectBank() : Persistent("EFFECTBANK") {
    // Default to VST3 effects
    for (int i = 0; i < MAX_EFFECT_COUNT; i++) {
        effects_[i] = new VST3Effect();
    }
}

EffectBank::~EffectBank() {
    for (int i = 0; i < MAX_EFFECT_COUNT; i++) {
        delete effects_[i];
    }
}

I_Effect *EffectBank::GetEffect(int i) {
    if (i < 0 || i >= MAX_EFFECT_COUNT) {
        return effects_[0];
    }
    return effects_[i];
}

void EffectBank::SetEffectType(int slot, EffectType type) {
    if (slot < 0 || slot >= MAX_EFFECT_COUNT) return;
    I_Effect *old = effects_[slot];
    if (!old) return;
    if (old->GetEffectType() == type) return;

    // Purge the old effect first
    old->Purge();

    // Clear any PlayerChannel references to the old effect before deleting
    // to prevent use-after-free in the audio thread's Render() call.
    PlayerMixer *pm = PlayerMixer::GetInstance();
    if (pm) pm->ReleaseEffect(old);

    // Create new effect of the requested type
    I_Effect *n = nullptr;
    switch (type) {
        case ET_LV2:  n = new LV2Effect(); break;
        case ET_VST3: n = new VST3Effect(); break;
        default: return;
    }

    effects_[slot] = n;
    delete old;
    n->Init();
}

EffectType EffectBank::GetEffectType(int slot) const {
    if (slot < 0 || slot >= MAX_EFFECT_COUNT) return ET_VST3;
    return effects_[slot]->GetEffectType();
}

void EffectBank::Init() {
    for (int i = 0; i < MAX_EFFECT_COUNT; i++) {
        effects_[i]->Init();
    }
}

void EffectBank::SaveContent(TiXmlNode *node) {
    char hex[3];
    for (int i = 0; i < MAX_EFFECT_COUNT; i++) {
        I_Effect *effect = effects_[i];
        if (!effect->IsEmpty()) {
            TiXmlElement data("EFFECT_SLOT");
            hex2char(i, hex);
            data.SetAttribute("ID", hex);

            // Save the effect's content (TYPE attribute is written by the effect)
            effect->SaveContent(&data);

            node->InsertEndChild(data);
        }
    }
}

void EffectBank::RestoreContent(TiXmlElement *element) {
    TiXmlElement *current = element->FirstChildElement();

    while (current) {
        if (!strcmp(current->Value(), "EFFECT_SLOT")) {
            const char *hexid = current->Attribute("ID");
            if (!hexid || strlen(hexid) < 2) {
                current = current->NextSiblingElement();
                continue;
            }
            unsigned char b1 = (c2h__(hexid[0])) << 4;
            unsigned char b2 = c2h__(hexid[1]);
            unsigned char id = b1 + b2;

            if (id < MAX_EFFECT_COUNT) {
                TiXmlElement *effectEl = current->FirstChildElement("EFFECT");
                if (effectEl) {
                    // Check the TYPE attribute to determine effect type
                    const char *typeAttr = effectEl->Attribute("TYPE");
                    if (typeAttr && strcmp(typeAttr, "VST3") == 0) {
                        // Ensure slot has a VST3Effect
                        if (effects_[id]->GetEffectType() != ET_VST3) {
                            PlayerMixer *pm = PlayerMixer::GetInstance();
                            if (pm) pm->ReleaseEffect(effects_[id]);
                            delete effects_[id];
                            effects_[id] = new VST3Effect();
                            effects_[id]->Init();
                        }
                    } else {
                        // Default/legacy: LV2 effect (no TYPE attr or TYPE="LV2")
                        if (effects_[id]->GetEffectType() != ET_LV2) {
                            PlayerMixer *pm = PlayerMixer::GetInstance();
                            if (pm) pm->ReleaseEffect(effects_[id]);
                            delete effects_[id];
                            effects_[id] = new LV2Effect();
                            effects_[id]->Init();
                        }
                    }
                    effects_[id]->RestoreContent(effectEl);
                }
            }
        }
        current = current->NextSiblingElement();
    }
}

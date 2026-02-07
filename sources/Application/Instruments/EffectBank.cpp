#include "EffectBank.h"
#include "Application/Utils/char.h"
#include "System/Console/Trace.h"

EffectBank::EffectBank() : Persistent("EFFECTBANK") {
    for (int i = 0; i < MAX_LV2EFFECT_COUNT; i++) {
        effects_[i] = new LV2Effect();
    }
}

EffectBank::~EffectBank() {
    for (int i = 0; i < MAX_LV2EFFECT_COUNT; i++) {
        delete effects_[i];
    }
}

LV2Effect *EffectBank::GetEffect(int i) {
    if (i < 0 || i >= MAX_LV2EFFECT_COUNT) {
        return effects_[0];
    }
    return effects_[i];
}

void EffectBank::Init() {
    for (int i = 0; i < MAX_LV2EFFECT_COUNT; i++) {
        effects_[i]->Init();
    }
}

void EffectBank::SaveContent(TiXmlNode *node) {
    char hex[3];
    for (int i = 0; i < MAX_LV2EFFECT_COUNT; i++) {
        LV2Effect *effect = effects_[i];
        if (!effect->IsEmpty()) {
            TiXmlElement data("EFFECT_SLOT");
            hex2char(i, hex);
            data.SetAttribute("ID", hex);

            // Use the effect's own SaveContent to serialize into the slot element
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

            if (id < MAX_LV2EFFECT_COUNT) {
                // Look for the EFFECT child element which contains the actual data
                TiXmlElement *effectEl = current->FirstChildElement("EFFECT");
                if (effectEl) {
                    effects_[id]->RestoreContent(effectEl);
                }
            }
        }
        current = current->NextSiblingElement();
    }
}

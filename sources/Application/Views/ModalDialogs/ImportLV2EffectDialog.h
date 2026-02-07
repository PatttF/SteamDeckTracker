#ifndef _IMPORT_LV2_EFFECT_DIALOG_H_
#define _IMPORT_LV2_EFFECT_DIALOG_H_

#include "Application/Views/BaseClasses/ModalView.h"
#include "Foundation/T_SimpleList.h"
#include <string>

struct LV2EffectPluginInfo {
    std::string uri;
    std::string name;
};

class LV2Effect;

class ImportLV2EffectDialog : public ModalView {
public:
    ImportLV2EffectDialog(View &view, LV2Effect *targetEffect);
    virtual ~ImportLV2EffectDialog();

    virtual void DrawView();
    virtual void OnPlayerUpdate(PlayerEventType, unsigned int currentTick);
    virtual void OnFocus();
    virtual void ProcessButtonMask(unsigned short mask, bool pressed);

protected:
    void loadPluginList();
    void warpToNext(int dir);
    void selectPlugin();

private:
    T_SimpleList<LV2EffectPluginInfo> pluginList_;
    int currentPlugin_;
    int topIndex_;
    LV2Effect *targetEffect_;
};

#endif

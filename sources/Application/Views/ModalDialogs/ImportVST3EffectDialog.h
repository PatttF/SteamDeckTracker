#ifndef _IMPORT_VST3_EFFECT_DIALOG_H_
#define _IMPORT_VST3_EFFECT_DIALOG_H_

#include "Application/Views/BaseClasses/ModalView.h"
#include "Application/Instruments/VST3Instrument.h"  // for VST3PluginInfo
#include <vector>

class I_Effect;

class ImportVST3EffectDialog : public ModalView {
public:
    ImportVST3EffectDialog(View &view, I_Effect *targetEffect);
    virtual ~ImportVST3EffectDialog();

    virtual void DrawView();
    virtual void OnPlayerUpdate(PlayerEventType, unsigned int);
    virtual void OnFocus();
    virtual void ProcessButtonMask(unsigned short mask, bool pressed);

protected:
    void warpToNext(int dir);
    void selectPlugin();

private:
    std::vector<VST3PluginInfo> pluginList_;
    int currentPlugin_;
    int topIndex_;
    I_Effect *targetEffect_;
};

#endif

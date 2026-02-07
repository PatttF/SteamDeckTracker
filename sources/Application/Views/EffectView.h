#ifndef _EFFECT_VIEW_H_
#define _EFFECT_VIEW_H_

#include "BaseClasses/FieldView.h"
#include "Foundation/Observable.h"
#include "ViewData.h"

class LV2Effect;
class UIActionField;

class EffectView : public FieldView, public I_Observer {
public:
    EffectView(GUIWindow &w, ViewData *data);
    virtual ~EffectView();

    virtual void ProcessButtonMask(unsigned short mask, bool pressed);
    virtual void DrawView();
    virtual void OnPlayerUpdate(PlayerEventType, unsigned int) {}
    virtual void OnFocus();
    void OnEffectPluginSelected();

protected:
    void warpToNext(int offset);
    void onEffectChange();
    void fillEffectParameters();
    void Update(Observable &o, I_ObservableData *d);

private:
    Project *project_;
    int currentEffect_;    // Currently selected effect slot (0-15)
    int lv2ScrollOffset_;
    char lv2PluginLabel_[80];
    char lv2ParamText_[40][40];
    UIActionField *lv2LoadField_;
    LV2Effect *current_;
};
#endif

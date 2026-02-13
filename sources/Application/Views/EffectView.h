#ifndef _EFFECT_VIEW_H_
#define _EFFECT_VIEW_H_

#include "BaseClasses/FieldView.h"
#include "Foundation/Observable.h"
#include "ViewData.h"
#include "Application/Instruments/I_Effect.h"

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
    int scrollOffset_;
    char pluginLabel_[80];
    char paramText_[40][40];
    UIActionField *loadField_;
    I_Effect *current_;
    int currentTypeIndex_;  // 0=VST3, 1=LV2 (picker index)
    int pendingTypeEffectIdx_;  // deferred type switch slot (-1 = none)
    EffectType pendingEffectType_;
    char bankLabel_[80];
    char presetLabel_[80];
};
#endif

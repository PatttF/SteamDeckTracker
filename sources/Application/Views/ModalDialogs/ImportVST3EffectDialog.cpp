#include "ImportVST3EffectDialog.h"
#include "Application/Instruments/VST3Effect.h"
#include "System/Console/Trace.h"
#include "UIFramework/BasicDatas/GUIEvent.h"
#include <cstring>

ImportVST3EffectDialog::ImportVST3EffectDialog(View &view, I_Effect *targetEffect)
    : ModalView(view) {
    currentPlugin_ = 0;
    topIndex_ = 0;
    targetEffect_ = targetEffect;

    // Scan for VST3 effect plugins (Fx category, not Instrument)
    pluginList_ = VST3Effect::ScanEffectPlugins();
    Trace::Log("VST3FX", "Found %d VST3 effect plugins", (int)pluginList_.size());
}

ImportVST3EffectDialog::~ImportVST3EffectDialog() {
}

void ImportVST3EffectDialog::OnFocus() {
    currentPlugin_ = 0;
    topIndex_ = 0;
    isDirty_ = true;
}

void ImportVST3EffectDialog::OnPlayerUpdate(PlayerEventType, unsigned int) {
}

void ImportVST3EffectDialog::DrawView() {
    SetWindow(62, 24);

    GUITextProperties props;

    int x = 0;
    int y = 1;
    int count = 0;
    int displayCount = 22;

    int total = (int)pluginList_.size();

    for (int index = topIndex_; index < total && count < displayCount; index++, count++) {
        if (index == currentPlugin_) {
            SetColor(CD_HILITE2);
            props.invert_ = true;
        } else {
            SetColor(CD_NORMAL);
            props.invert_ = false;
        }

        DrawString(x, y, pluginList_[index].name.c_str(), props);
        y++;
    }

    if (total == 0) {
        SetColor(CD_NORMAL);
        props.invert_ = false;
        DrawString(x, y, "No VST3 effect plugins found", props);
    }
}

void ImportVST3EffectDialog::warpToNext(int dir) {
    int size = (int)pluginList_.size();
    if (size == 0) return;

    currentPlugin_ += dir;
    if (currentPlugin_ < 0) currentPlugin_ = 0;
    if (currentPlugin_ >= size) currentPlugin_ = size - 1;

    if (currentPlugin_ < topIndex_) {
        topIndex_ = currentPlugin_;
    } else if (currentPlugin_ >= topIndex_ + 22) {
        topIndex_ = currentPlugin_ - 21;
    }

    isDirty_ = true;
}

void ImportVST3EffectDialog::selectPlugin() {
    int size = (int)pluginList_.size();
    if (size == 0 || currentPlugin_ < 0 || currentPlugin_ >= size) return;

    VST3PluginInfo &info = pluginList_[currentPlugin_];

    if (targetEffect_ && targetEffect_->GetEffectType() == ET_VST3) {
        VST3Effect *vst3fx = (VST3Effect *)targetEffect_;
        vst3fx->SetPlugin(info.path.c_str(), info.classId);
    }

    EndModal(1);
}

void ImportVST3EffectDialog::ProcessButtonMask(unsigned short mask, bool pressed) {
    if (!pressed) return;

    if (mask & EPBM_UP) warpToNext(-1);
    if (mask & EPBM_DOWN) warpToNext(1);
    if (mask & EPBM_A) selectPlugin();
    if (mask & EPBM_B) EndModal(0);
}

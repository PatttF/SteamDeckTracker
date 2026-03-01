#include "ImportVST3Dialog.h"
#include "Application/Instruments/InstrumentBank.h"
#include "Application/Instruments/VST3Instrument.h"
#include "System/Console/Trace.h"
#include "UIFramework/BasicDatas/GUIEvent.h"
#include <cstring>

ImportVST3Dialog::ImportVST3Dialog(View &view) : ModalView(view) {
    currentPlugin_ = 0;
    topIndex_ = 0;
    toInstr_ = view.viewData_->currentInstrument_;

    // Scan for available VST3 plugins
    pluginList_ = VST3Instrument::ScanPlugins();
    
}

ImportVST3Dialog::~ImportVST3Dialog() {
}

void ImportVST3Dialog::OnFocus() {
    currentPlugin_ = 0;
    topIndex_ = 0;
    isDirty_ = true;
}

void ImportVST3Dialog::OnPlayerUpdate(PlayerEventType, unsigned int) {
}

void ImportVST3Dialog::DrawView() {
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

    // Show "no plugins found" message if list is empty
    if (total == 0) {
        SetColor(CD_NORMAL);
        props.invert_ = false;
        DrawString(x, y, "No VST3 plugins found", props);
    }
}

void ImportVST3Dialog::warpToNext(int dir) {
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

void ImportVST3Dialog::selectPlugin() {
    int size = (int)pluginList_.size();
    if (size == 0 || currentPlugin_ < 0 || currentPlugin_ >= size) return;

    VST3PluginInfo &info = pluginList_[currentPlugin_];

    // Get the instrument and set the plugin
    InstrumentBank *bank = viewData_->project_->GetInstrumentBank();
    I_Instrument *instr = bank->GetInstrument(toInstr_);

    if (instr->GetType() == IT_VST3) {
        VST3Instrument *vst3instr = (VST3Instrument *)instr;
        vst3instr->SetPlugin(info.path.c_str(), info.classId);
    }

    EndModal(0);
}

void ImportVST3Dialog::ProcessButtonMask(unsigned short mask, bool pressed) {
    if (!pressed) return;

    if (mask & EPBM_UP) {
        warpToNext(-1);
    }
    if (mask & EPBM_DOWN) {
        warpToNext(1);
    }
    if (mask & EPBM_A) {
        selectPlugin();
    }
    if (mask & EPBM_B) {
        EndModal(0);
    }
}

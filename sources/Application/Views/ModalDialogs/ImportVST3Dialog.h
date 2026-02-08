#ifndef _IMPORT_VST3_DIALOG_H_
#define _IMPORT_VST3_DIALOG_H_

#include "Application/Views/BaseClasses/ModalView.h"
#include "Foundation/T_SimpleList.h"
#include "Application/Instruments/VST3Instrument.h"

class ImportVST3Dialog : public ModalView {
public:
    ImportVST3Dialog(View &view);
    virtual ~ImportVST3Dialog();

    virtual void DrawView();
    virtual void OnPlayerUpdate(PlayerEventType, unsigned int);
    virtual void OnFocus();
    virtual void ProcessButtonMask(unsigned short mask, bool pressed);

    void SetTargetInstrument(int idx) { toInstr_ = idx; }

protected:
    void warpToNext(int dir);
    void selectPlugin();

private:
    std::vector<VST3PluginInfo> pluginList_;
    int currentPlugin_;
    int topIndex_;
    int toInstr_;
};

#endif

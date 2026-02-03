#ifndef _IMPORT_LV2_DIALOG_H_
#define _IMPORT_LV2_DIALOG_H_

#include "Application/Views/BaseClasses/ModalView.h"
#include "Foundation/T_SimpleList.h"
#include <string>

struct LV2PluginInfo {
    std::string uri;
    std::string name;
};

class ImportLV2Dialog : public ModalView {
public:
    ImportLV2Dialog(View &view);
    virtual ~ImportLV2Dialog();

    virtual void DrawView();
    virtual void OnPlayerUpdate(PlayerEventType, unsigned int currentTick);
    virtual void OnFocus();
    virtual void ProcessButtonMask(unsigned short mask, bool pressed);

    // Allow setting the target instrument index programmatically
    void SetTargetInstrument(int idx) { toInstr_ = idx; }

protected:
    void loadPluginList();
    void warpToNext(int dir);
    void selectPlugin();

    // Callback for channel selection modal
    static void ChannelSelectCallback(View &v, ModalView &dialog);

private:
    T_SimpleList<LV2PluginInfo> pluginList_;
    int currentPlugin_;
    int topIndex_;
    int toInstr_;

    // Pending plugin awaiting channel selection confirmation
    std::string pendingPluginURI_;
};

#endif

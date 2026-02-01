#include "ImportLV2Dialog.h"
#include "Application/Instruments/InstrumentBank.h"
#include "Application/Instruments/LV2Instrument.h"
#include "Application/Model/Config.h"
#include "System/Console/Trace.h"
#include "UIFramework/BasicDatas/GUIEvent.h"
#include <cstring>
#include <lilv/lilv.h>

#ifdef __linux__
#include <dirent.h>
#include <sys/stat.h>
#endif

ImportLV2Dialog::ImportLV2Dialog(View &view) : ModalView(view) {
    currentPlugin_ = 0;
    topIndex_ = 0;
    toInstr_ = view.viewData_->currentInstrument_;
    loadPluginList();
}

ImportLV2Dialog::~ImportLV2Dialog() {
    // Clean up plugin list - items were allocated with new
    IteratorPtr<LV2PluginInfo> it(pluginList_.GetIterator());
    for (it->Begin(); !it->IsDone(); it->Next()) {
        LV2PluginInfo &info = it->CurrentItem();
        delete &info;
    }
    pluginList_.Empty();
}

void ImportLV2Dialog::loadPluginList() {
    // Use lilv to discover LV2 plugins
    LilvWorld* world = lilv_world_new();
    if (!world) {
        Trace::Error("Failed to create LV2 world");
        return;
    }
    
    lilv_world_load_all(world);
    const LilvPlugins* plugins = lilv_world_get_all_plugins(world);
    
    LILV_FOREACH(plugins, i, plugins) {
        const LilvPlugin* plugin = lilv_plugins_get(plugins, i);
        
        // Get plugin URI
        const LilvNode* uri_node = lilv_plugin_get_uri(plugin);
        const char* uri = lilv_node_as_uri(uri_node);
        
        // Get plugin name
        LilvNode* name_node = lilv_plugin_get_name(plugin);
        const char* name = lilv_node_as_string(name_node);
        
        // Add to list
        LV2PluginInfo *info = new LV2PluginInfo();
        info->uri = uri;
        info->name = name ? name : uri;
        pluginList_.Insert(*info);
        
        lilv_node_free(name_node);
    }
    
    lilv_world_free(world);
}

void ImportLV2Dialog::OnFocus() {
    currentPlugin_ = 0;
    topIndex_ = 0;
    isDirty_ = true;
}

void ImportLV2Dialog::OnPlayerUpdate(PlayerEventType, unsigned int) {
}

void ImportLV2Dialog::DrawView() {
    SetWindow(30, 13);

    GUITextProperties props;

    // Draw plugin list
    int x = 1;
    int y = 1;
    int count = 0;
    int displayCount = 18; // Show 18 plugins at a time

    IteratorPtr<LV2PluginInfo> it(pluginList_.GetIterator());
    int index = 0;
    
    for (it->Begin(); !it->IsDone(); it->Next()) {
        if (index >= topIndex_ && count < displayCount) {
            LV2PluginInfo &info = it->CurrentItem();
            
            if (index == currentPlugin_) {
                SetColor(CD_HILITE2);
                props.invert_ = true;
            } else {
                SetColor(CD_NORMAL);
                props.invert_ = false;
            }
            
            DrawString(x, y, info.name.c_str(), props);
            y++;
            count++;
        }
        index++;
    }
}

void ImportLV2Dialog::warpToNext(int dir) {
    int size = 0;
    IteratorPtr<LV2PluginInfo> it(pluginList_.GetIterator());
    for (it->Begin(); !it->IsDone(); it->Next()) {
        size++;
    }
    
    if (size == 0) return;
    
    currentPlugin_ += dir;
    if (currentPlugin_ < 0) currentPlugin_ = 0;
    if (currentPlugin_ >= size) currentPlugin_ = size - 1;
    
    // Adjust top index for scrolling
    if (currentPlugin_ < topIndex_) {
        topIndex_ = currentPlugin_;
    } else if (currentPlugin_ >= topIndex_ + 10) {
        topIndex_ = currentPlugin_ - 9;
    }
    
    isDirty_ = true;
}

void ImportLV2Dialog::selectPlugin() {
    // Get the selected plugin
    IteratorPtr<LV2PluginInfo> it(pluginList_.GetIterator());
    int index = 0;
    
    for (it->Begin(); !it->IsDone(); it->Next()) {
        if (index == currentPlugin_) {
            LV2PluginInfo &info = it->CurrentItem();
            
            // Get the instrument and set the plugin
            InstrumentBank *bank = viewData_->project_->GetInstrumentBank();
            I_Instrument *instr = bank->GetInstrument(toInstr_);
            
            if (instr->GetType() == IT_LV2) {
                LV2Instrument *lv2instr = (LV2Instrument *)instr;
                lv2instr->SetPlugin(info.uri.c_str());
            }
            
            EndModal(0);
            return;
        }
        index++;
    }
}

void ImportLV2Dialog::ProcessButtonMask(unsigned short mask, bool pressed) {
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

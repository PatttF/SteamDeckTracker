#include "ImportLV2EffectDialog.h"
#include "Application/Instruments/LV2Effect.h"
#include "System/Console/Trace.h"
#include <cstring>
#include <lilv/lilv.h>
#include <lv2/core/lv2.h>

ImportLV2EffectDialog::ImportLV2EffectDialog(View &view, LV2Effect *targetEffect)
    : ModalView(view) {
    currentPlugin_ = 0;
    topIndex_ = 0;
    targetEffect_ = targetEffect;
    loadPluginList();
}

ImportLV2EffectDialog::~ImportLV2EffectDialog() {
    IteratorPtr<LV2EffectPluginInfo> it(pluginList_.GetIterator());
    for (it->Begin(); !it->IsDone(); it->Next()) {
        LV2EffectPluginInfo &info = it->CurrentItem();
        delete &info;
    }
    pluginList_.Empty();
}

// Helper: check if a plugin class is a subclass of lv2:InstrumentPlugin
// by walking up the class hierarchy
static bool isInstrumentClass(LilvWorld *world, const LilvPluginClass *pclass) {
    const LilvNode *classUri = lilv_plugin_class_get_uri(pclass);
    LilvNode *instrUri = lilv_new_uri(world, LV2_CORE__InstrumentPlugin);
    bool result = lilv_node_equals(classUri, instrUri);
    lilv_node_free(instrUri);
    if (result) return true;

    // Walk parent chain
    const LilvNode *parentUri = lilv_plugin_class_get_parent_uri(pclass);
    if (parentUri) {
        LilvNode *instrNode = lilv_new_uri(world, LV2_CORE__InstrumentPlugin);
        if (lilv_node_equals(parentUri, instrNode)) {
            lilv_node_free(instrNode);
            return true;
        }
        lilv_node_free(instrNode);
    }
    return false;
}

void ImportLV2EffectDialog::loadPluginList() {
    LilvWorld *world = lilv_world_new();
    if (!world) return;

    lilv_world_load_all(world);
    const LilvPlugins *plugins = lilv_world_get_all_plugins(world);

    // We want audio effects - plugins with audio input AND audio output ports,
    // but NOT instrument plugins. Effect plugins include: reverbs, delays,
    // compressors, EQs, distortion, etc.
    LilvNode *audio_class = lilv_new_uri(world, LILV_URI_AUDIO_PORT);
    LilvNode *input_class = lilv_new_uri(world, LILV_URI_INPUT_PORT);
    LilvNode *output_class = lilv_new_uri(world, LILV_URI_OUTPUT_PORT);

    LILV_FOREACH(plugins, i, plugins) {
        const LilvPlugin *plugin = lilv_plugins_get(plugins, i);

        // Skip instrument plugins
        const LilvPluginClass *pclass = lilv_plugin_get_class(plugin);
        if (isInstrumentClass(world, pclass)) continue;

        // Check if the plugin has both audio input and audio output ports
        uint32_t numPorts = lilv_plugin_get_num_ports(plugin);
        bool hasAudioIn = false;
        bool hasAudioOut = false;

        for (uint32_t p = 0; p < numPorts; p++) {
            const LilvPort *port = lilv_plugin_get_port_by_index(plugin, p);
            if (!port) continue;

            if (lilv_port_is_a(plugin, port, audio_class)) {
                if (lilv_port_is_a(plugin, port, input_class)) hasAudioIn = true;
                if (lilv_port_is_a(plugin, port, output_class)) hasAudioOut = true;
            }
        }

        // Only include plugins that process audio (have both in and out)
        if (!hasAudioIn || !hasAudioOut) continue;

        const LilvNode *uri_node = lilv_plugin_get_uri(plugin);
        const char *uri = lilv_node_as_uri(uri_node);

        LilvNode *name_node = lilv_plugin_get_name(plugin);
        const char *name = lilv_node_as_string(name_node);

        LV2EffectPluginInfo *info = new LV2EffectPluginInfo();
        info->uri = uri;
        info->name = name ? name : uri;
        pluginList_.Insert(*info);

        lilv_node_free(name_node);
    }

    lilv_node_free(audio_class);
    lilv_node_free(input_class);
    lilv_node_free(output_class);
    lilv_world_free(world);
}

void ImportLV2EffectDialog::OnFocus() {
    currentPlugin_ = 0;
    topIndex_ = 0;
    isDirty_ = true;
}

void ImportLV2EffectDialog::OnPlayerUpdate(PlayerEventType, unsigned int) {}

void ImportLV2EffectDialog::DrawView() {
    SetWindow(20, 13);

    GUITextProperties props;

    int x = 0;
    int y = 1;
    int count = 0;
    int displayCount = 18;

    IteratorPtr<LV2EffectPluginInfo> it(pluginList_.GetIterator());
    int index = 0;

    for (it->Begin(); !it->IsDone(); it->Next()) {
        if (index >= topIndex_ && count < displayCount) {
            LV2EffectPluginInfo &info = it->CurrentItem();

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

void ImportLV2EffectDialog::warpToNext(int dir) {
    int size = 0;
    IteratorPtr<LV2EffectPluginInfo> it(pluginList_.GetIterator());
    for (it->Begin(); !it->IsDone(); it->Next()) {
        size++;
    }

    if (size == 0) return;

    currentPlugin_ += dir;
    if (currentPlugin_ < 0) currentPlugin_ = 0;
    if (currentPlugin_ >= size) currentPlugin_ = size - 1;

    if (currentPlugin_ < topIndex_) {
        topIndex_ = currentPlugin_;
    } else if (currentPlugin_ >= topIndex_ + 10) {
        topIndex_ = currentPlugin_ - 9;
    }

    isDirty_ = true;
}

void ImportLV2EffectDialog::selectPlugin() {
    IteratorPtr<LV2EffectPluginInfo> it(pluginList_.GetIterator());
    int index = 0;

    for (it->Begin(); !it->IsDone(); it->Next()) {
        if (index == currentPlugin_) {
            LV2EffectPluginInfo &info = it->CurrentItem();

            if (targetEffect_) {
                targetEffect_->SetForcedOutputChannels(2);
                targetEffect_->SetPlugin(info.uri.c_str());
            }

            EndModal(1);
            return;
        }
        index++;
    }
}

void ImportLV2EffectDialog::ProcessButtonMask(unsigned short mask, bool pressed) {
    if (!pressed) return;

    if (mask & EPBM_UP) warpToNext(-1);
    if (mask & EPBM_DOWN) warpToNext(1);
    if (mask & EPBM_A) selectPlugin();
    if (mask & EPBM_B) EndModal(0);
}

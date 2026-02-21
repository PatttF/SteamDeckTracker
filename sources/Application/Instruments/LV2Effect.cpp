#include "LV2Effect.h"
#include "Application/Model/Config.h"
#include "Foundation/Variables/WatchedVariable.h"
#include "System/Console/Trace.h"
#include "Services/Audio/Audio.h"
#include <cstring>
#include <cstdlib>
#include <lilv/lilv.h>
#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/urid/urid.h>
#include <lv2/options/options.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/patch/patch.h>

// Use the same global URID map and features from LV2Instrument
extern LV2_URID urid_map(LV2_URID_Map_Handle handle, const char *uri);
extern LV2_URID_Map g_uridMapFeature;
extern const char* urid_unmap(LV2_URID_Unmap_Handle handle, LV2_URID urid);
extern LV2_URID_Unmap g_uridUnmapFeature;
extern LV2_Feature g_mapFeature;
extern LV2_Feature g_unmapFeature;
extern LV2_Options_Option g_optionsArray[];
extern LV2_Feature g_optionsFeature;
extern LV2_Options_Interface g_optionsInterface;
extern LV2_Feature g_optionsInterfaceFeature;
extern LV2_Feature g_boundedFeature;
extern LV2_URID g_midiEventUrid;
extern LV2_URID g_atomSequenceUrid;
extern LV2_URID g_atomFloatUrid;

LV2Effect::LV2Effect() {
    strcpy(name_, "Effect");
    pluginURI_[0] = '\0';
    world_ = nullptr;
    plugin_ = nullptr;
    pluginInstance_ = nullptr;
    audioBufferL_ = nullptr;
    audioBufferR_ = nullptr;
    audioInputL_ = nullptr;
    audioInputR_ = nullptr;
    audioDummyBuffer_ = nullptr;
    bufferSize_ = 0;
    audioInputPortL_ = -1;
    audioInputPortR_ = -1;
    audioOutputPortL_ = -1;
    audioOutputPortR_ = -1;
    midiInputPort_ = -1;
    midiBuffer_ = nullptr;
    midiBufferSize_ = 0;
    isActivated_ = false;
    forcedOutputChannels_ = 0;
    cachedPatchSetUrid_ = 0;
    cachedPatchPropertyUrid_ = 0;
    cachedPatchValueUrid_ = 0;

    // Add volume and wet/dry variables
    Variable *vol = new Variable("volume", LV2FX_VOLUME, 0xFF);
    Insert(vol);
    Variable *wet = new Variable("wet", LV2FX_WETDRY, 0xFF);
    Insert(wet);
}

LV2Effect::~LV2Effect() {
    Purge();
}

bool LV2Effect::Init() {
    return true;
}

const char *LV2Effect::GetName() const {
    if (IsEmpty()) {
        return "-- no effect --";
    }
    return name_;
}

void LV2Effect::Purge() {
    // Remove observers from parameter variables
    for (size_t i = 0; i < parameters_.size(); ++i) {
        if (parameters_[i].variable) {
            WatchedVariable *wv = dynamic_cast<WatchedVariable*>(parameters_[i].variable);
            if (wv) wv->RemoveObserver(*this);
        }
    }
    // Lock while modifying plugin state to prevent audio thread access
    pluginMutex_.Lock();
    cleanupPlugin();
    pluginURI_[0] = '\0';
    strcpy(name_, "Effect");
    pluginMutex_.Unlock();
    parameters_.clear();
    controlValues_.clear();
}

void LV2Effect::SetForcedOutputChannels(int count) {
    forcedOutputChannels_ = count;
}

void LV2Effect::SetPlugin(const char *uri) {
    // Clean up any existing plugin (Purge locks internally)
    Purge();

    strncpy(pluginURI_, uri, sizeof(pluginURI_) - 1);
    pluginURI_[sizeof(pluginURI_) - 1] = '\0';

    loadPlugin();
}

void LV2Effect::loadPlugin() {
    if (pluginURI_[0] == '\0') return;

    // Ensure global LV2 URIDs are initialized (they may not be if no LV2Instrument was loaded first)
    if (g_midiEventUrid == 0) {
        g_midiEventUrid = urid_map(nullptr, "http://lv2plug.in/ns/ext/midi#MidiEvent");
    }
    if (g_atomSequenceUrid == 0) {
        g_atomSequenceUrid = urid_map(nullptr, "http://lv2plug.in/ns/ext/atom#Sequence");
    }
    if (g_atomFloatUrid == 0) {
        g_atomFloatUrid = urid_map(nullptr, LV2_ATOM__Float);
    }

    // Ensure options array is filled (keys/types may be zero if no LV2Instrument loaded yet)
    if (g_optionsArray[0].key == 0) {
        g_optionsArray[0].key   = urid_map(nullptr, LV2_BUF_SIZE__maxBlockLength);
        g_optionsArray[0].size  = sizeof(uint32_t);
        g_optionsArray[0].type  = urid_map(nullptr, LV2_ATOM__Int);
        static uint32_t s_maxBlock_fx = 131072u;
        g_optionsArray[0].value = &s_maxBlock_fx;

        g_optionsArray[1].key   = urid_map(nullptr, LV2_BUF_SIZE__nominalBlockLength);
        g_optionsArray[1].size  = sizeof(uint32_t);
        g_optionsArray[1].type  = urid_map(nullptr, LV2_ATOM__Int);
        static uint32_t s_nominalBlock_fx = 1024u;
        g_optionsArray[1].value = &s_nominalBlock_fx;

        g_optionsArray[2].key   = urid_map(nullptr, LV2_BUF_SIZE__minBlockLength);
        g_optionsArray[2].size  = sizeof(uint32_t);
        g_optionsArray[2].type  = urid_map(nullptr, LV2_ATOM__Int);
        static uint32_t s_minBlock_fx = 64u;
        g_optionsArray[2].value = &s_minBlock_fx;
        // g_optionsArray[3] stays as zero terminator
    }

    // Create a new lilv world for this effect
    LilvWorld *world = lilv_world_new();
    if (!world) {
        Trace::Error("LV2Effect: Failed to create lilv world");
        return;
    }
    lilv_world_load_all(world);
    world_ = world;

    const LilvPlugins *plugins = lilv_world_get_all_plugins(world);
    LilvNode *uri_node = lilv_new_uri(world, pluginURI_);

    const LilvPlugin *plugin = lilv_plugins_get_by_uri(plugins, uri_node);
    lilv_node_free(uri_node);

    if (!plugin) {
        Trace::Error("LV2Effect: Plugin not found: %s", pluginURI_);
        return;
    }

    plugin_ = (void *)plugin;

    // Get plugin name
    LilvNode *name_node = lilv_plugin_get_name(plugin);
    if (name_node) {
        strncpy(name_, lilv_node_as_string(name_node), sizeof(name_) - 1);
        name_[sizeof(name_) - 1] = '\0';
        lilv_node_free(name_node);
    }

    // Load bundle resource
    const LilvNode *bundle = lilv_plugin_get_bundle_uri(plugin);
    if (bundle) {
        lilv_world_load_resource(world, lilv_plugin_get_uri(plugin));
        lilv_world_load_bundle(world, bundle);
    }

    // Discover ports
    uint32_t numPorts = lilv_plugin_get_num_ports(plugin);
    LilvNode *audio_class = lilv_new_uri(world, LILV_URI_AUDIO_PORT);
    LilvNode *input_class = lilv_new_uri(world, LILV_URI_INPUT_PORT);
    LilvNode *output_class = lilv_new_uri(world, LILV_URI_OUTPUT_PORT);
    LilvNode *atom_class = lilv_new_uri(world, "http://lv2plug.in/ns/ext/atom#AtomPort");
    LilvNode *midi_class = lilv_new_uri(world, LV2_MIDI__MidiEvent);
    LilvNode *optional_class = lilv_new_uri(world, LV2_CORE__connectionOptional);

    int audioInCount = 0;
    int audioOutCount = 0;

    for (uint32_t i = 0; i < numPorts; i++) {
        const LilvPort *port = lilv_plugin_get_port_by_index(plugin, i);

        if (lilv_port_is_a(plugin, port, audio_class)) {
            if (lilv_port_is_a(plugin, port, input_class)) {
                if (audioInCount == 0) audioInputPortL_ = i;
                else if (audioInCount == 1) audioInputPortR_ = i;
                audioInCount++;
            } else if (lilv_port_is_a(plugin, port, output_class)) {
                audioOutputPorts_.push_back(i);
                if (audioOutCount == 0) audioOutputPortL_ = i;
                else if (audioOutCount == 1) audioOutputPortR_ = i;
                audioOutCount++;
            }
        } else if (lilv_port_is_a(plugin, port, atom_class)) {
            if (lilv_port_is_a(plugin, port, input_class)) {
                if (midiInputPort_ < 0) midiInputPort_ = i;
            }
        }
    }

    lilv_node_free(audio_class);
    lilv_node_free(input_class);
    lilv_node_free(output_class);
    lilv_node_free(atom_class);
    lilv_node_free(midi_class);
    lilv_node_free(optional_class);

    // Force stereo if requested
    if (forcedOutputChannels_ == 2 && audioOutCount > 2) {
        audioOutCount = 2;
    }

    

    // Set up features array
    const LV2_Feature *features[] = {
        &g_mapFeature,
        &g_unmapFeature,
        &g_optionsFeature,
        &g_optionsInterfaceFeature,
        &g_boundedFeature,
        nullptr
    };

    // Instantiate the plugin with actual audio driver sample rate
    double pluginRate = (double)Audio::GetInstance()->GetSampleRate();
    
    LilvInstance *instance = lilv_plugin_instantiate(plugin, pluginRate, features);
    if (!instance) {
        Trace::Error("LV2Effect: Failed to instantiate plugin: %s", pluginURI_);
        // Try minimal features
        const LV2_Feature *minFeatures[] = {
            &g_mapFeature,
            &g_unmapFeature,
            nullptr
        };
        instance = lilv_plugin_instantiate(plugin, pluginRate, minFeatures);
        if (!instance) {
            Trace::Error("LV2Effect: Failed to instantiate even with minimal features: %s", pluginURI_);
            return;
        }
    }

    pluginInstance_ = instance;

    // Discover parameters
    discoverParameters();

    // Allocate buffers and connect ports (use 2048 to match LV2Instrument and
    // cover typical block sizes, avoiding reallocation in the audio callback)
    bufferSize_ = 2048;
    audioBufferL_ = new float[bufferSize_];
    audioBufferR_ = new float[bufferSize_];
    audioInputL_ = new float[bufferSize_];
    audioInputR_ = new float[bufferSize_];
    audioDummyBuffer_ = new float[bufferSize_];
    memset(audioBufferL_, 0, bufferSize_ * sizeof(float));
    memset(audioBufferR_, 0, bufferSize_ * sizeof(float));
    memset(audioInputL_, 0, bufferSize_ * sizeof(float));
    memset(audioInputR_, 0, bufferSize_ * sizeof(float));
    memset(audioDummyBuffer_, 0, bufferSize_ * sizeof(float));

    // Allocate MIDI buffer if needed
    if (midiInputPort_ >= 0) {
        midiBufferSize_ = 65536;
        midiBuffer_ = new uint8_t[midiBufferSize_];
        memset(midiBuffer_, 0, midiBufferSize_);
    }

    connectPorts(bufferSize_);

    // Activate under lock — this is the point where ProcessAudio
    // transitions from seeing an empty plugin to a live one
    pluginMutex_.Lock();
    lilv_instance_activate(instance);
    isActivated_ = true;
    pluginMutex_.Unlock();
}

void LV2Effect::cleanupPlugin() {
    if (pluginInstance_) {
        if (isActivated_) {
            lilv_instance_deactivate((LilvInstance *)pluginInstance_);
            isActivated_ = false;
        }
        lilv_instance_free((LilvInstance *)pluginInstance_);
        pluginInstance_ = nullptr;
    }

    delete[] audioBufferL_;
    audioBufferL_ = nullptr;
    delete[] audioBufferR_;
    audioBufferR_ = nullptr;
    delete[] audioInputL_;
    audioInputL_ = nullptr;
    delete[] audioInputR_;
    audioInputR_ = nullptr;
    delete[] audioDummyBuffer_;
    audioDummyBuffer_ = nullptr;
    delete[] midiBuffer_;
    midiBuffer_ = nullptr;
    midiBufferSize_ = 0;

    // Clean up atom buffers
    for (size_t i = 0; i < atomInputBuffers_.size(); i++) {
        delete[] atomInputBuffers_[i];
    }
    atomInputBuffers_.clear();
    atomInputBufferSizes_.clear();

    for (size_t i = 0; i < atomOutputBuffers_.size(); i++) {
        delete[] atomOutputBuffers_[i];
    }
    atomOutputBuffers_.clear();
    atomOutputBufferSizes_.clear();

    if (world_) {
        lilv_world_free((LilvWorld *)world_);
        world_ = nullptr;
    }

    plugin_ = nullptr;
    bufferSize_ = 0;
    audioInputPortL_ = -1;
    audioInputPortR_ = -1;
    audioOutputPortL_ = -1;
    audioOutputPortR_ = -1;
    midiInputPort_ = -1;
    audioOutputPorts_.clear();
}

void LV2Effect::connectPorts(int bufferSize) {
    if (!plugin_ || !pluginInstance_) return;

    const LilvPlugin *plugin = (const LilvPlugin *)plugin_;
    uint32_t numPorts = lilv_plugin_get_num_ports(plugin);

    LilvNode *audio_class = lilv_new_uri((LilvWorld *)world_, LILV_URI_AUDIO_PORT);
    LilvNode *control_class = lilv_new_uri((LilvWorld *)world_, LILV_URI_CONTROL_PORT);
    LilvNode *input_class = lilv_new_uri((LilvWorld *)world_, LILV_URI_INPUT_PORT);
    LilvNode *output_class = lilv_new_uri((LilvWorld *)world_, LILV_URI_OUTPUT_PORT);
    LilvNode *atom_class = lilv_new_uri((LilvWorld *)world_, "http://lv2plug.in/ns/ext/atom#AtomPort");

    // Free any previously allocated atom input buffers before reassigning
    for (size_t j = 0; j < atomInputBuffers_.size(); ++j) {
        delete[] atomInputBuffers_[j];
    }
    atomInputBuffers_.assign(numPorts, nullptr);
    atomInputBufferSizes_.assign(numPorts, 0);

    if (portControlStorage_.size() < numPorts) {
        portControlStorage_.resize(numPorts, 0.0f);
    }

    LilvInstance *instance = (LilvInstance *)pluginInstance_;

    for (uint32_t i = 0; i < numPorts; i++) {
        const LilvPort *port = lilv_plugin_get_port_by_index(plugin, i);
        if (!port) continue;

        if (lilv_port_is_a(plugin, port, audio_class)) {
            if (lilv_port_is_a(plugin, port, input_class)) {
                if ((int)i == audioInputPortL_ && audioInputL_)
                    lilv_instance_connect_port(instance, i, audioInputL_);
                else if ((int)i == audioInputPortR_ && audioInputR_)
                    lilv_instance_connect_port(instance, i, audioInputR_);
                else
                    lilv_instance_connect_port(instance, i, audioInputL_ ? audioInputL_ : nullptr);
            } else if (lilv_port_is_a(plugin, port, output_class)) {
                if ((int)i == audioOutputPortL_ && audioBufferL_)
                    lilv_instance_connect_port(instance, i, audioBufferL_);
                else if ((int)i == audioOutputPortR_ && audioBufferR_)
                    lilv_instance_connect_port(instance, i, audioBufferR_);
                else
                    lilv_instance_connect_port(instance, i, audioDummyBuffer_ ? audioDummyBuffer_ : nullptr);
            }
            continue;
        }

        if (lilv_port_is_a(plugin, port, control_class)) {
            if (portControlStorage_.size() <= i) portControlStorage_.resize(i + 1, 0.0f);
            lilv_instance_connect_port(instance, i, &portControlStorage_[i]);
            continue;
        }

        if (lilv_port_is_a(plugin, port, atom_class)) {
            bool isInput = lilv_port_is_a(plugin, port, input_class);
            bool isOutput = lilv_port_is_a(plugin, port, output_class);

            if ((int)i == midiInputPort_ && midiBuffer_) {
                lilv_instance_connect_port(instance, i, midiBuffer_);
                continue;
            }

            size_t bufSize = (midiBufferSize_ > 0) ? midiBufferSize_ : 8192;

            if (isOutput) {
                if (atomOutputBuffers_.size() <= i) {
                    atomOutputBuffers_.resize(i + 1, nullptr);
                    atomOutputBufferSizes_.resize(i + 1, 0);
                }
                if (atomOutputBuffers_[i]) delete[] atomOutputBuffers_[i];
                atomOutputBuffers_[i] = new uint8_t[bufSize];
                atomOutputBufferSizes_[i] = bufSize;
                LV2_Atom_Sequence *seq = (LV2_Atom_Sequence *)atomOutputBuffers_[i];
                seq->atom.type = g_atomSequenceUrid;
                seq->atom.size = bufSize - sizeof(LV2_Atom);
                seq->body.unit = 0;
                seq->body.pad = 0;
                lilv_instance_connect_port(instance, i, atomOutputBuffers_[i]);
            } else if (isInput) {
                if (atomInputBuffers_[i]) {
                    delete[] atomInputBuffers_[i];
                    atomInputBuffers_[i] = nullptr;
                }
                atomInputBuffers_[i] = new uint8_t[bufSize];
                memset(atomInputBuffers_[i], 0, bufSize);
                atomInputBufferSizes_[i] = bufSize;
                lilv_instance_connect_port(instance, i, atomInputBuffers_[i]);
            }
            continue;
        }
    }

    // Write parameter default values into port control storage
    for (size_t pi = 0; pi < parameters_.size(); pi++) {
        int pidx = parameters_[pi].portIndex;
        if (pidx >= 0 && pidx < (int)portControlStorage_.size() && !parameters_[pi].isAtomPort) {
            portControlStorage_[pidx] = parameters_[pi].currentValue;
        }
    }

    lilv_node_free(audio_class);
    lilv_node_free(control_class);
    lilv_node_free(input_class);
    lilv_node_free(output_class);
    lilv_node_free(atom_class);
}

void LV2Effect::discoverParameters() {
    if (!plugin_ || !world_) return;

    const LilvPlugin *plugin = (const LilvPlugin *)plugin_;
    LilvWorld *world = (LilvWorld *)world_;

    // This follows the same logic as LV2Instrument::discoverParameters()
    // First discover control ports
    LilvNode *control_class = lilv_new_uri(world, LILV_URI_CONTROL_PORT);
    LilvNode *input_class = lilv_new_uri(world, LILV_URI_INPUT_PORT);
    LilvNode *output_class = lilv_new_uri(world, LILV_URI_OUTPUT_PORT);
    LilvNode *atom_class = lilv_new_uri(world, "http://lv2plug.in/ns/ext/atom#AtomPort");
    LilvNode *enumeration_prop = lilv_new_uri(world, LV2_CORE__enumeration);
    LilvNode *connectionOptional_prop = lilv_new_uri(world, LV2_CORE__connectionOptional);
    LilvNode *patchWritable = lilv_new_uri(world, "http://lv2plug.in/ns/ext/patch#writable");
    LilvNode *rdfsRange = lilv_new_uri(world, "http://www.w3.org/2000/01/rdf-schema#range");
    LilvNode *rdfsLabel = lilv_new_uri(world, "http://www.w3.org/2000/01/rdf-schema#label");
    LilvNode *lv2_portGroup = lilv_new_uri(world, "http://lv2plug.in/ns/lv2core#portGroup");
    LilvNode *lv2_name = lilv_new_uri(world, LILV_NS_LV2 "name");

    uint32_t numPorts = lilv_plugin_get_num_ports(plugin);
    parameters_.clear();

    // Get default values
    float *defaults = new float[numPorts];
    lilv_plugin_get_port_ranges_float(plugin, nullptr, nullptr, defaults);

    for (uint32_t i = 0; i < numPorts; i++) {
        const LilvPort *port = lilv_plugin_get_port_by_index(plugin, i);
        if (!port) continue;

        if (!lilv_port_is_a(plugin, port, control_class)) continue;

        bool isOutput = lilv_port_is_a(plugin, port, output_class);
        bool isAtomPort = lilv_port_is_a(plugin, port, atom_class);

        LV2PluginParameter param;
        LilvNode *nameNode = lilv_port_get_name(plugin, port);
        param.name = nameNode ? lilv_node_as_string(nameNode) : "???";
        if (nameNode) lilv_node_free(nameNode);

        // Get port group
        LilvNodes *groups = lilv_port_get_value(plugin, port, lv2_portGroup);
        if (groups) {
            const LilvNode *groupNode = lilv_nodes_get_first(groups);
            if (groupNode) {
                LilvNodes *groupNames = lilv_world_find_nodes(world, groupNode, lv2_name, nullptr);
                if (groupNames) {
                    const LilvNode *gn = lilv_nodes_get_first(groupNames);
                    if (gn) param.groupName = lilv_node_as_string(gn);
                    lilv_nodes_free(groupNames);
                }
            }
            lilv_nodes_free(groups);
        }

        LilvNode *minNode = nullptr, *maxNode = nullptr;
        lilv_port_get_range(plugin, port, nullptr, &minNode, &maxNode);
        param.minValue = minNode ? lilv_node_as_float(minNode) : 0.0f;
        param.maxValue = maxNode ? lilv_node_as_float(maxNode) : 1.0f;
        if (minNode) lilv_node_free(minNode);
        if (maxNode) lilv_node_free(maxNode);

        param.defaultValue = defaults[i];
        param.currentValue = param.defaultValue;
        param.portIndex = i;
        param.isOutput = isOutput;
        param.isAtomPort = isAtomPort;
        param.isEnumeration = lilv_port_has_property(plugin, port, enumeration_prop);

        // Get scale points
        LilvScalePoints *sps = lilv_port_get_scale_points(plugin, port);
        if (sps) {
            LILV_FOREACH(scale_points, spi, sps) {
                const LilvScalePoint *sp = lilv_scale_points_get(sps, spi);
                LV2ScalePoint lsp;
                lsp.value = lilv_node_as_float(lilv_scale_point_get_value(sp));
                const LilvNode *labelNode = lilv_scale_point_get_label(sp);
                lsp.label = labelNode ? lilv_node_as_string(labelNode) : "";
                param.scalePoints.push_back(lsp);
            }
            lilv_scale_points_free(sps);
        }

        // Skip output control ports entirely — they are for the plugin to
        // write to (e.g. level meters) and should not be stored as parameters
        if (isOutput) {
            continue;
        }

        // Create a WatchedVariable for UI binding (scaled 0-127)
        int scaledDefault = 0;
        if (param.maxValue != param.minValue) {
            scaledDefault = (int)((param.defaultValue - param.minValue) / (param.maxValue - param.minValue) * 127.0f);
        }
        if (scaledDefault < 0) scaledDefault = 0;
        if (scaledDefault > 127) scaledDefault = 127;

        char varName[64];
        snprintf(varName, sizeof(varName), "FX_%u", i);
        FourCC varId = MAKE_FOURCC('F', 'X', (char)('0' + (i / 10)), (char)('0' + (i % 10)));
        if (i >= 100) {
            varId = MAKE_FOURCC('F', 'Y', (char)('0' + ((i / 10) % 10)), (char)('0' + (i % 10)));
        }

        WatchedVariable *wv = new WatchedVariable(varName, varId, scaledDefault);
        Insert(wv);
        wv->AddObserver(*this);
        param.variable = wv;

        parameters_.push_back(param);
    }

    // Also discover patch:writable parameters (like Surge XT uses)
    LilvNodes *writableParams = lilv_world_find_nodes(world,
        lilv_plugin_get_uri(plugin), patchWritable, nullptr);
    if (writableParams) {
        LILV_FOREACH(nodes, wi, writableParams) {
            const LilvNode *paramNode = lilv_nodes_get(writableParams, wi);
            if (!paramNode) continue;

            LV2PluginParameter param;
            param.resourceURI = lilv_node_as_uri(paramNode);

            LilvNodes *labels = lilv_world_find_nodes(world, paramNode, rdfsLabel, nullptr);
            if (labels) {
                const LilvNode *lbl = lilv_nodes_get_first(labels);
                if (lbl) param.name = lilv_node_as_string(lbl);
                lilv_nodes_free(labels);
            }
            if (param.name.empty()) param.name = param.resourceURI;

            LilvNodes *ranges = lilv_world_find_nodes(world, paramNode, rdfsRange, nullptr);
            if (ranges) {
                lilv_nodes_free(ranges);
            }

            param.minValue = 0.0f;
            param.maxValue = 1.0f;
            param.defaultValue = 0.0f;
            param.currentValue = 0.0f;
            param.portIndex = -1;
            param.isOutput = false;
            param.isAtomPort = true;
            param.isEnumeration = false;

            int idx = (int)parameters_.size();
            char varName[64];
            snprintf(varName, sizeof(varName), "FXR_%d", idx);
            FourCC varId = MAKE_FOURCC('R', 'X', (char)('0' + ((idx / 10) % 10)), (char)('0' + (idx % 10)));

            WatchedVariable *wv = new WatchedVariable(varName, varId, 0);
            Insert(wv);
            wv->AddObserver(*this);
            param.variable = wv;

            parameters_.push_back(param);
        }
        lilv_nodes_free(writableParams);
    }

    delete[] defaults;

    lilv_node_free(control_class);
    lilv_node_free(input_class);
    lilv_node_free(output_class);
    lilv_node_free(atom_class);
    lilv_node_free(enumeration_prop);
    lilv_node_free(connectionOptional_prop);
    lilv_node_free(patchWritable);
    lilv_node_free(rdfsRange);
    lilv_node_free(rdfsLabel);
    lilv_node_free(lv2_portGroup);
    lilv_node_free(lv2_name);

    // Write control port defaults (do NOT send patch:Set here)
    for (size_t pi = 0; pi < parameters_.size(); pi++) {
        int pidx = parameters_[pi].portIndex;
        if (pidx >= 0 && pidx < (int)portControlStorage_.size() && !parameters_[pi].isAtomPort) {
            portControlStorage_[pidx] = parameters_[pi].currentValue;
        }
    }

    // Apply any pending variable values from project load
    for (auto &pv : pendingParamValues_) {
        for (size_t i = 0; i < parameters_.size(); i++) {
            if (parameters_[i].variable && pv.first == parameters_[i].variable->GetName()) {
                int val = atoi(pv.second.c_str());
                parameters_[i].variable->SetInt(val);
                break;
            }
        }
    }
    pendingParamValues_.clear();
}

bool LV2Effect::ProcessAudio(fixed *buffer, int sampleCount, int wetDry) {
    // Try to acquire the plugin mutex — if the UI thread is loading/unloading,
    // skip processing for this buffer (pass through dry signal)
    if (!pluginMutex_.TryLock()) {
        return true;  // return true so dry signal passes through unchanged
    }

    if (!pluginInstance_ || !audioBufferL_ || !audioBufferR_ || !audioInputL_ || !audioInputR_) {
        pluginMutex_.Unlock();
        
        return false;
    }

    if (!buffer || sampleCount <= 0) {
        pluginMutex_.Unlock();
        return false;
    }

    if (!isActivated_) {
        pluginMutex_.Unlock();
        
        return false;
    }

    // Grow buffers if needed (keep existing allocation to avoid RT realloc)
    if (sampleCount > bufferSize_) {
        // Reallocation unavoidable — log warning as this is a real-time violation
        
        delete[] audioBufferL_;
        delete[] audioBufferR_;
        delete[] audioInputL_;
        delete[] audioInputR_;
        delete[] audioDummyBuffer_;
        bufferSize_ = sampleCount;
        audioBufferL_ = new float[bufferSize_];
        audioBufferR_ = new float[bufferSize_];
        audioInputL_ = new float[bufferSize_];
        audioInputR_ = new float[bufferSize_];
        audioDummyBuffer_ = new float[bufferSize_];
        connectPorts(bufferSize_);
    }

    // Round block size to even to satisfy plugins that process in pairs
    int runSize = sampleCount & ~1;
    if (runSize < 2) runSize = 2;
    if (runSize > sampleCount) runSize = sampleCount;

    // Deinterleave input from fixed-point stereo interleaved to float L/R
    // Q15 fixed-point stores full-scale 16-bit audio as ±~1 billion (i2fp).
    // fp2fl converts that to ±32768.0 float, but LV2 plugins expect ±1.0,
    // so we divide by 32768 to normalize.
    const float toFloat = 1.0f / (32768.0f * 32768.0f);  // combined fp2fl + normalize
    for (int i = 0; i < runSize; i++) {
        audioInputL_[i] = (float)buffer[i * 2] * toFloat;
        audioInputR_[i] = (float)buffer[i * 2 + 1] * toFloat;
    }

    // Clear output buffers before running plugin to prevent stale data artifacts
    memset(audioBufferL_, 0, runSize * sizeof(float));
    memset(audioBufferR_, 0, runSize * sizeof(float));

    // Set up MIDI buffer as empty sequence (no MIDI events for effects)
    if (midiBuffer_) {
        LV2_Atom_Sequence *seq = (LV2_Atom_Sequence *)midiBuffer_;
        seq->atom.type = g_atomSequenceUrid;
        seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
        seq->body.unit = 0;
        seq->body.pad = 0;

        // Write any pending atom events (parameter changes) under lock
        pendingEventsMutex_.Lock();
        if (!pendingAtomEvents_.empty()) {
            uint8_t *buf = (uint8_t *)LV2_ATOM_CONTENTS(LV2_Atom_Sequence, seq);
            size_t offset = 0;
            size_t capacity = midiBufferSize_ - sizeof(LV2_Atom_Sequence);

            for (auto &ae : pendingAtomEvents_) {
                size_t padded = sizeof(LV2_Atom_Event) + ((ae.data.size() + 7) & ~7);
                if (offset + padded > capacity) break;
                LV2_Atom_Event *event = (LV2_Atom_Event *)(buf + offset);
                event->time.frames = 0;
                event->body.size = (uint32_t)ae.data.size();
                event->body.type = ae.type;
                memcpy(LV2_ATOM_BODY(&event->body), ae.data.data(), ae.data.size());
                offset += padded;
            }
            seq->atom.size = sizeof(LV2_Atom_Sequence_Body) + offset;
            pendingAtomEvents_.clear();
        }
        pendingEventsMutex_.Unlock();
    }

    // Re-initialize non-MIDI atom INPUT buffers as empty sequences
    for (size_t ai = 0; ai < atomInputBuffers_.size(); ai++) {
        if (atomInputBuffers_[ai] && (int)ai != midiInputPort_) {
            LV2_Atom_Sequence *seq = (LV2_Atom_Sequence *)atomInputBuffers_[ai];
            seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
            seq->atom.type = g_atomSequenceUrid;
            seq->body.unit = 0;
            seq->body.pad = 0;
        }
    }

    // Re-initialize atom output buffers
    for (size_t oi = 0; oi < atomOutputBuffers_.size(); oi++) {
        if (atomOutputBuffers_[oi]) {
            LV2_Atom_Sequence *seq = (LV2_Atom_Sequence *)atomOutputBuffers_[oi];
            seq->atom.type = g_atomSequenceUrid;
            seq->atom.size = atomOutputBufferSizes_[oi] - sizeof(LV2_Atom);
            seq->body.unit = 0;
            seq->body.pad = 0;
        }
    }

    // Clear dummy buffer before plugin run
    if (audioDummyBuffer_) {
        memset(audioDummyBuffer_, 0, sampleCount * sizeof(float));
    }

    // Run the plugin with even-rounded block size
    lilv_instance_run((LilvInstance *)pluginInstance_, runSize);

    // Duplicate the last sample for the trailing odd sample
    if (runSize < sampleCount) {
        audioBufferL_[runSize] = audioBufferL_[runSize - 1];
        audioBufferR_[runSize] = audioBufferR_[runSize - 1];
    }

    // Sanitise plugin output: zero any NaN/Inf samples.
    // LV2 plugins with feedback networks (reverb, delay, resonant
    // filters) can produce NaN/Inf which would poison all downstream
    // processing.  We use a bitwise check that survives -ffast-math.
    {
        union { float f; uint32_t u; } chk;
        for (int i = 0; i < sampleCount; i++) {
            chk.f = audioBufferL_[i];
            if ((chk.u & 0x7F800000u) == 0x7F800000u) audioBufferL_[i] = 0.0f;
            chk.f = audioBufferR_[i];
            if ((chk.u & 0x7F800000u) == 0x7F800000u) audioBufferR_[i] = 0.0f;
        }
    }

    // If mono plugin (no right output port), duplicate left to right
    if (audioOutputPortR_ == -1 && audioBufferL_ && audioBufferR_) {
        memcpy(audioBufferR_, audioBufferL_, sampleCount * sizeof(float));
    }

    // Wet/dry mix and convert back to fixed-point stereo interleaved
    // Plugin output is ±1.0 float; Q15 full-scale is i2fp(32767) ≈ 1 billion.
    // So multiply by 32768*32768 = 1073741824 to convert back.
    const float fromFloat = 32768.0f * 32768.0f;
    float wet = wetDry / 255.0f;
    float dry = 1.0f - wet;

    for (int i = 0; i < sampleCount; i++) {
        float dryL = (float)buffer[i * 2] * toFloat;
        float dryR = (float)buffer[i * 2 + 1] * toFloat;
        float wetL = audioBufferL_[i];
        float wetR = audioBufferR_[i];

        float mixL = dryL * dry + wetL * wet;
        float mixR = dryR * dry + wetR * wet;

        // Clamp to ±1.0 to prevent fixed-point overflow / clipping
        if (mixL > 1.0f) mixL = 1.0f;
        else if (mixL < -1.0f) mixL = -1.0f;
        if (mixR > 1.0f) mixR = 1.0f;
        else if (mixR < -1.0f) mixR = -1.0f;

        buffer[i * 2]     = (fixed)(mixL * fromFloat);
        buffer[i * 2 + 1] = (fixed)(mixR * fromFloat);
    }

    pluginMutex_.Unlock();
    return true;
}

void LV2Effect::SetParameterValue(int paramIndex, float value) {
    if (paramIndex < 0 || paramIndex >= (int)parameters_.size()) return;

    parameters_[paramIndex].currentValue = value;

    int pidx = parameters_[paramIndex].portIndex;

    if (pidx >= 0 && pidx < (int)portControlStorage_.size()) {
        // Direct control port
        portControlStorage_[pidx] = value;
    } else if (!parameters_[paramIndex].resourceURI.empty() && midiInputPort_ >= 0 && midiBuffer_) {
        // Resource-backed parameter: queue a patch:Set atom event
        // Cache URIDs on first use so we never call urid_map() on the audio thread
        if (cachedPatchSetUrid_ == 0) {
            cachedPatchSetUrid_ = urid_map(nullptr, LV2_PATCH__Set);
            cachedPatchPropertyUrid_ = urid_map(nullptr, LV2_PATCH__property);
            cachedPatchValueUrid_ = urid_map(nullptr, LV2_PATCH__value);
        }

        LV2_Atom_Forge forge;
        lv2_atom_forge_init(&forge, &g_uridMapFeature);

        const size_t TMP_SIZE = 1024;
        uint8_t tmp[TMP_SIZE];
        lv2_atom_forge_set_buffer(&forge, tmp, TMP_SIZE);

        LV2_Atom_Forge_Frame frame;
        lv2_atom_forge_object(&forge, &frame, 0, cachedPatchSetUrid_);
        lv2_atom_forge_key(&forge, cachedPatchPropertyUrid_);
        lv2_atom_forge_urid(&forge, urid_map(nullptr, parameters_[paramIndex].resourceURI.c_str()));
        lv2_atom_forge_key(&forge, cachedPatchValueUrid_);
        lv2_atom_forge_float(&forge, value);
        lv2_atom_forge_pop(&forge, &frame);

        size_t totalSize = forge.offset;
        if (totalSize > 0 && totalSize <= TMP_SIZE) {
            LV2_Atom *atom = (LV2_Atom *)tmp;
            PendingAtomEvent pae;
            pae.data.assign(tmp + sizeof(LV2_Atom), tmp + sizeof(LV2_Atom) + atom->size);
            pae.type = atom->type;
            pae.destPortIndex = midiInputPort_;
            pendingEventsMutex_.Lock();
            pendingAtomEvents_.push_back(pae);
            pendingEventsMutex_.Unlock();
        }
    }
}

void LV2Effect::SyncHostVolume(int hostVol) {
    double norm = (double)hostVol / 255.0;
    if (norm < 0.0) norm = 0.0;
    if (norm > 1.0) norm = 1.0;

    // Update wrapper variable
    Variable *volVar = FindVariable(LV2FX_VOLUME);
    if (volVar) volVar->SetInt(hostVol);

    // Find native parameters that look like gain/volume/output
    auto matchesKeyword = [](const std::string &s) {
        std::string t = s;
        for (auto &c : t) c = (char)tolower(c);
        return (t.find("volume") != std::string::npos) ||
               (t.find("gain") != std::string::npos) ||
               (t.find("output") != std::string::npos) ||
               (t.find("level") != std::string::npos) ||
               (t.find("out") != std::string::npos);
    };

    for (size_t i = 0; i < parameters_.size(); ++i) {
        const std::string &pname = parameters_[i].name;
        if (!pname.empty() && matchesKeyword(pname)) {
            // Scale into parameter real range
            float realValue = parameters_[i].minValue + (float)norm * (parameters_[i].maxValue - parameters_[i].minValue);
            SetParameterValue((int)i, realValue);
            if (parameters_[i].variable) {
                // LV2 UI variables are mapped to 0..127 in this host
                int maxVal = 127;
                int intVal = (int)(norm * maxVal + 0.5f);
                parameters_[i].variable->SetInt(intVal);
            }

            // update all plausible matches
        }
    }
}

std::string LV2Effect::GetParameterScalePointLabel(int paramIndex, float value) const {
    if (paramIndex < 0 || paramIndex >= (int)parameters_.size()) return "";
    const LV2PluginParameter &p = parameters_[paramIndex];
    for (auto &sp : p.scalePoints) {
        float diff = sp.value - value;
        if (diff < 0) diff = -diff;
        if (diff < 0.001f) return sp.label;
    }
    return "";
}

const EffectParameter* LV2Effect::GetEffectParameter(int index) const {
    if (index < 0 || index >= (int)parameters_.size()) return nullptr;
    // Build a static EffectParameter on the fly from the LV2PluginParameter
    static thread_local EffectParameter ep;
    const LV2PluginParameter &p = parameters_[index];
    ep.name = p.name;
    ep.groupName = p.groupName;
    ep.minValue = p.minValue;
    ep.maxValue = p.maxValue;
    ep.currentValue = p.currentValue;
    ep.variable = p.variable;
    ep.scalePoints.clear();
    for (auto &sp : p.scalePoints) {
        EffectParameter::ScalePoint esp;
        esp.value = sp.value;
        esp.label = sp.label;
        ep.scalePoints.push_back(esp);
    }
    return &ep;
}

void LV2Effect::Update(Observable &o, I_ObservableData *d) {
    Variable *v = dynamic_cast<Variable *>(&o);
    if (!v) return;

    for (size_t i = 0; i < parameters_.size(); i++) {
        if (parameters_[i].variable == v) {
            int scaled = v->GetInt();
            float realValue = parameters_[i].minValue +
                (scaled / 127.0f) * (parameters_[i].maxValue - parameters_[i].minValue);
            SetParameterValue((int)i, realValue);
            break;
        }
    }
}

void LV2Effect::StorePendingVariable(const char *name, const char *value) {
    pendingParamValues_[name] = value;
}

void LV2Effect::SaveContent(TiXmlNode *node) {
    // Save plugin URI
    TiXmlElement effect("EFFECT");
    effect.SetAttribute("URI", pluginURI_);

    // Lock to prevent the audio thread from modifying parameters_ concurrently.
    // ProcessAudio uses TryLock so it will just pass through dry signal.
    pluginMutex_.Lock();
    // Save parameter values
    for (size_t i = 0; i < parameters_.size(); i++) {
        if (parameters_[i].variable) {
            TiXmlElement param("PARAM");
            param.SetAttribute("NAME", parameters_[i].variable->GetName());
            param.SetAttribute("VALUE", parameters_[i].variable->GetInt());
            effect.InsertEndChild(param);
        }
    }
    pluginMutex_.Unlock();

    // Save volume and wet/dry
    Variable *vol = FindVariable(LV2FX_VOLUME);
    if (vol) {
        TiXmlElement v("VOL");
        v.SetAttribute("VALUE", vol->GetInt());
        effect.InsertEndChild(v);
    }
    Variable *wet = FindVariable(LV2FX_WETDRY);
    if (wet) {
        TiXmlElement w("WET");
        w.SetAttribute("VALUE", wet->GetInt());
        effect.InsertEndChild(w);
    }

    node->InsertEndChild(effect);
}

void LV2Effect::RestoreContent(TiXmlElement *element) {
    const char *uri = element->Attribute("URI");
    if (uri && uri[0] != '\0') {
        // Store pending variable values before loading the plugin
        TiXmlElement *paramEl = element->FirstChildElement("PARAM");
        while (paramEl) {
            const char *name = paramEl->Attribute("NAME");
            const char *value = paramEl->Attribute("VALUE");
            if (name && value) {
                StorePendingVariable(name, value);
            }
            paramEl = paramEl->NextSiblingElement("PARAM");
        }

        // Restore volume and wet
        TiXmlElement *volEl = element->FirstChildElement("VOL");
        if (volEl) {
            const char *val = volEl->Attribute("VALUE");
            if (val) {
                Variable *v = FindVariable(LV2FX_VOLUME);
                if (v) v->SetInt(atoi(val));
            }
        }
        TiXmlElement *wetEl = element->FirstChildElement("WET");
        if (wetEl) {
            const char *val = wetEl->Attribute("VALUE");
            if (val) {
                Variable *v = FindVariable(LV2FX_WETDRY);
                if (v) v->SetInt(atoi(val));
            }
        }

        SetForcedOutputChannels(2);
        SetPlugin(uri);
    }
}

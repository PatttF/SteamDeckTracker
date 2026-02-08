#ifndef _LV2_EFFECT_H_
#define _LV2_EFFECT_H_

#include "Foundation/Variables/VariableContainer.h"
#include "Foundation/Observable.h"
#include "Application/Persistency/Persistent.h"
#include "Application/Utils/fixed.h"
#include <vector>
#include <string>
#include <cstdint>
#include <map>
#include <lv2/urid/urid.h>
#include "System/Process/SysMutex.h"

// Re-use the scale point and parameter structs from LV2Instrument
#include "LV2Instrument.h"

// Effect slot count
#define MAX_LV2EFFECT_COUNT 0x10

// Variable IDs for effect parameters
#define LV2FX_VOLUME   MAKE_FOURCC('F','V','O','L')
#define LV2FX_WETDRY   MAKE_FOURCC('F','W','E','T')

class LV2Effect : public VariableContainer, public I_Observer {
public:
    LV2Effect();
    virtual ~LV2Effect();

    // I_Observer: notified when WatchedVariables change
    virtual void Update(Observable &o, I_ObservableData *d) override;

    bool Init();

    bool IsEmpty() const { return pluginURI_[0] == '\0'; }
    const char *GetName() const;
    const char *GetPluginURI() const { return pluginURI_; }

    // Plugin management
    void SetPlugin(const char *uri);
    void Purge();

    // Audio processing: process interleaved stereo fixed-point buffer in-place
    // Returns true if audio was modified
    bool ProcessAudio(fixed *buffer, int sampleCount, int wetDry = 255);

    // Parameter access
    int GetParameterCount() const { return (int)parameters_.size(); }
    const LV2PluginParameter* GetParameter(int index) const {
        if (index >= 0 && index < (int)parameters_.size()) {
            return &parameters_[index];
        }
        return nullptr;
    }
    void SetParameterValue(int index, float value);
    void SetForcedOutputChannels(int count);

    // Scale point label lookup
    std::string GetParameterScalePointLabel(int paramIndex, float value) const;

    // Persistence support
    void StorePendingVariable(const char *name, const char *value);
    void SaveContent(TiXmlNode *node);
    void RestoreContent(TiXmlElement *element);

private:
    char name_[80];
    char pluginURI_[256];

    // LV2 plugin state
    void *world_;
    void *plugin_;
    void *pluginInstance_;
    float *audioBufferL_;
    float *audioBufferR_;
    float *audioInputL_;
    float *audioInputR_;
    float *audioDummyBuffer_;
    int bufferSize_;

    // Port indices
    int audioInputPortL_;
    int audioInputPortR_;
    int audioOutputPortL_;
    int audioOutputPortR_;
    int midiInputPort_;
    std::vector<int> audioOutputPorts_;
    int forcedOutputChannels_;

    // MIDI/atom buffers (for plugins that require atom ports)
    uint8_t *midiBuffer_;
    size_t midiBufferSize_;
    std::vector<uint8_t*> atomOutputBuffers_;
    std::vector<size_t> atomOutputBufferSizes_;
    std::vector<uint8_t*> atomInputBuffers_;
    std::vector<size_t> atomInputBufferSizes_;

    // Pending atom events for parameter changes
    struct PendingAtomEvent {
        std::vector<uint8_t> data;
        LV2_URID type;
        int destPortIndex;
    };
    std::vector<PendingAtomEvent> pendingAtomEvents_;
    SysMutex pendingEventsMutex_;

    // Cached URIDs for patch:Set atom events (avoid calling urid_map on audio thread)
    LV2_URID cachedPatchSetUrid_;
    LV2_URID cachedPatchPropertyUrid_;
    LV2_URID cachedPatchValueUrid_;

    // Plugin parameters
    std::vector<LV2PluginParameter> parameters_;
    std::vector<float> controlValues_;
    std::vector<float> portControlStorage_;
    bool isActivated_;

    // Mutex protecting plugin lifecycle (load/unload vs audio processing)
    SysMutex pluginMutex_;

    // Pending variable values from project load
    std::map<std::string, std::string> pendingParamValues_;

    // Helper methods
    void discoverParameters();
    void loadPlugin();
    void cleanupPlugin();
    void connectPorts(int bufferSize);
};

#endif

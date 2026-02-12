#ifndef _VST3_EFFECT_H_
#define _VST3_EFFECT_H_

#include "I_Effect.h"
#include "VST3Instrument.h"  // for VST3PluginInfo, VST3PluginParameter structs
#include "System/Process/SysMutex.h"
#include <vector>
#include <string>
#include <cstdint>
#include <map>

// Variable IDs for VST3 effect parameters
#define VST3FX_VOLUME   MAKE_FOURCC('V','F','V','L')
#define VST3FX_WETDRY   MAKE_FOURCC('V','F','W','T')
#define VST3FX_BANK     MAKE_FOURCC('V','F','B','K')
#define VST3FX_PRESET   MAKE_FOURCC('V','F','P','R')

class VST3Effect : public I_Effect {
public:
    VST3Effect();
    virtual ~VST3Effect();

    // I_Observer
    virtual void Update(Observable &o, I_ObservableData *d) override;

    // I_Effect interface
    virtual EffectType GetEffectType() const override { return ET_VST3; }
    virtual bool Init() override;
    virtual void Purge() override;
    virtual bool IsEmpty() const override { return pluginPath_[0] == '\0'; }
    virtual const char *GetName() const override;
    virtual void SetForcedOutputChannels(int count) override;
    virtual bool ProcessAudio(fixed *buffer, int sampleCount, int wetDry = 255) override;
    virtual int GetParameterCount() const override { return (int)effectParams_.size(); }
    virtual const EffectParameter* GetEffectParameter(int index) const override;
    virtual std::string GetParameterScalePointLabel(int paramIndex, float value) const override;
    virtual void SaveContent(TiXmlNode *node) override;
    virtual void RestoreContent(TiXmlElement *element) override;

    // VST3-specific plugin management
    void SetPlugin(const char *path, const uint8_t classId[16]);
    const char *GetPluginPath() const { return pluginPath_; }
    const uint8_t *GetClassId() const { return pluginClassId_; }

    // Scanning: discover VST3 effect plugins (Fx category, not Instrument)
    static std::vector<VST3PluginInfo> ScanEffectPlugins();

    // Access raw VST3 parameter info (for UI field display)
    const VST3PluginParameter *GetVST3Parameter(int index) const;

    // Preset/program support (via IUnitInfo)
    int GetBankCount() const { return (int)programLists_.size(); }
    const char *GetBankName(int bankIdx) const;
    int GetCurrentBank() const { return currentBank_; }
    void SetCurrentBank(int bankIdx);
    int GetPresetCount() const;
    const char *GetPresetName(int presetIdx) const;
    int GetCurrentPreset() const { return currentPreset_; }
    void SetPreset(int presetIdx);

    // Store pending variable value for restore
    void StorePendingVariable(const char *name, const char *value);

private:
    char name_[80];
    char pluginPath_[512];
    uint8_t pluginClassId_[16];

    // VST3 plugin state (opaque pointers)
    void *moduleHandle_;
    void *pluginFactory_;
    void *component_;
    void *audioProcessor_;
    void *editController_;
    bool componentIsController_;

    // Audio buffers
    float *audioBufferL_;
    float *audioBufferR_;
    float *audioInputL_;
    float *audioInputR_;
    int bufferSize_;

    // Plugin parameters (VST3-native)
    std::vector<VST3PluginParameter> parameters_;

    // Generic effect parameters (for I_Effect interface)
    std::vector<EffectParameter> effectParams_;

    // Pending parameter changes
    struct PendingParamChange {
        uint32_t paramId;
        double value;
    };
    std::vector<PendingParamChange> pendingParamChanges_;
    SysMutex pendingEventsMutex_;

    // Pending variable values from project load
    std::map<std::string, std::string> pendingParamValues_;

    // Pre-allocated scratch for audio thread
    std::vector<PendingParamChange> renderLocalParams_;

    bool isActive_;
    bool isProcessing_;
    int forcedOutputChannels_;

    // Mutex protecting plugin lifecycle
    SysMutex pluginMutex_;

    // Helper methods
    void loadPlugin();
    void cleanupPlugin();
    void discoverParameters();
    void discoverPresets();
    void setupProcessing(int bufferSize);
    void buildEffectParams();

    // Program lists / presets (discovered via IUnitInfo)
    std::vector<VST3ProgramList> programLists_;
    int currentBank_;
    int currentPreset_;
    int32_t programChangeParamIdx_;
};

#endif

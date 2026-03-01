#ifndef _VST3_INSTRUMENT_H_
#define _VST3_INSTRUMENT_H_

#include "I_Instrument.h"
#include "Application/Model/Song.h"
#include "System/Process/SysMutex.h"
#include <vector>
#include <string>
#include <cstdint>
#include <map>

// VST3 instrument parameters (FourCC identifiers)
#define VST3IP_PLUGIN       MAKE_FOURCC('V','P','L','G')
#define VST3IP_VOLUME       MAKE_FOURCC('V','V','O','L')
#define VST3IP_PAN          MAKE_FOURCC('V','P','A','N')
#define VST3IP_TABLE        MAKE_FOURCC('V','T','B','L')
#define VST3IP_TABLEAUTO    MAKE_FOURCC('V','T','B','A')
#define VST3IP_BANK         MAKE_FOURCC('V','B','N','K')
#define VST3IP_PRESET       MAKE_FOURCC('V','P','R','S')

// Forward declarations for VST3 COM interfaces (we hold them as void* to
// avoid leaking Steinberg headers into the rest of the codebase).
// The actual Steinberg::Vst::* types are cast in the .cpp file.

struct VST3PluginParameter {
    std::string name;
    std::string units;
    double minValue;        // plain (denormalized) value
    double maxValue;
    double defaultValue;
    double currentValue;
    uint32_t paramId;       // VST3 ParamID
    int32_t stepCount;      // 0=continuous, 1=toggle, N=discrete
    Variable *variable;     // Variable for UI binding
    bool isReadOnly;
    bool isBypass;
    bool isList;            // flag: is an enum/list param
};

// Info struct used by ImportVST3Dialog for discovered plugins
struct VST3PluginInfo {
    std::string name;
    std::string path;       // .vst3 bundle path
    std::string classIdStr; // CID as hex string for identification
    uint8_t classId[16];    // raw TUID (CID)
};

// Info about a single program list (bank) discovered via IUnitInfo
struct VST3ProgramList {
    int32_t listId;                     // ProgramListID
    std::string name;                   // list/bank name
    std::vector<std::string> programs;  // program names
};

// File-based preset entry (for plugins that don't expose presets via IUnitInfo)
struct VST3FilePreset {
    std::string name;       // display name (filename without extension)
    std::string filePath;   // full filesystem path to preset file
};

// Known plugin preset directory mapping
struct VST3PluginPresetMapping {
    const char *pluginNameSubstring;    // substring to match in plugin name
    const char *directories[6];         // directories to scan (nullptr terminated)
    const char *extension;              // file extension (e.g. ".fxp")
    int headerSkipBytes;                // bytes to skip at start of file (60 for FXP)
    const char *extension2;             // optional 2nd extension (nullptr if unused)
    int headerSkipBytes2;               // header skip for 2nd extension
};

class VST3Instrument : public I_Instrument, public I_Observer {

public:
    VST3Instrument();
    virtual ~VST3Instrument();

    // I_Observer implementation: notified when WatchedVariables change
    virtual void Update(Observable &o, I_ObservableData *d) override;

    virtual bool Init();

    // Start & stop the instrument (MIDI note on/off)
    virtual bool Start(int channel, unsigned char note, bool retrigger = true);
    virtual void Stop(int channel);

    // Render audio — fills interleaved stereo fixed-point buffer
    virtual bool Render(int channel, fixed *buffer, int size, bool updateTick);
    virtual void ProcessCommand(int channel, FourCC cc, ushort value);
    virtual void QueueMidiEvent(unsigned char status, unsigned char data1, unsigned char data2);

    virtual bool IsInitialized();

    virtual bool IsEmpty() { return pluginPath_[0] == '\0'; }

    virtual InstrumentType GetType() { return IT_VST3; }

    virtual const char *GetName();

    virtual void OnStart();

    virtual void Purge();

    virtual int GetTable();
    virtual bool GetTableAutomation();
    virtual void GetTableState(TableSaveState &state);
    virtual void SetTableState(TableSaveState &state);

    // VST3-specific methods
    void SetPlugin(const char *path, const uint8_t classId[16]);
    const char *GetPluginPath() const { return pluginPath_; }

    // Parameter access
    int GetParameterCount() const { return (int)parameters_.size(); }
    const VST3PluginParameter* GetParameter(int index) const {
        if (index >= 0 && index < (int)parameters_.size()) {
            return &parameters_[index];
        }
        return nullptr;
    }
    void SetParameterValue(int index, double normalizedValue);

    // Get display string for a parameter at a given normalized value
    // (calls IEditController::getParamStringByValue for labels like "Saw", "-6 dB")
    std::string GetParameterDisplayString(int index, double normalizedValue) const;

    // Preset/program support (via IUnitInfo)
    int GetBankCount() const { return (int)programLists_.size(); }
    const char *GetBankName(int bankIdx) const;
    int GetCurrentBank() const { return currentBank_; }
    void SetCurrentBank(int bankIdx);
    int GetPresetCount() const;        // program count for current bank
    int GetPresetCountForBank(int bankIdx) const;
    const char *GetPresetName(int presetIdx) const;
    int GetCurrentPreset() const { return currentPreset_; }
    void SetPreset(int presetIdx);     // selects program via kIsProgramChange param

    // Save current plugin state as a native preset file
    bool savePresetToFile(const std::string &filePath);
    // Get the directory of the current bank (for saving presets)
    std::string getCurrentBankDirectory() const;
    // Get the native file extension for this plugin's presets
    const char *getPresetExtension() const;
    // Returns true if this plugin has file-based presets (save is supported)
    bool canSavePreset() const;
    // Refresh preset file list after saving
    void refreshPresets();

    // Store a variable value read from project file when variable doesn't yet exist
    void StorePendingVariable(const char *name, const char *value);

    // Full plugin state save/restore (binary blob via IComponent/IEditController)
    std::string GetComponentStateBase64();
    std::string GetControllerStateBase64();
    bool RestoreComponentState(const std::string &base64Data);
    bool RestoreControllerState(const std::string &base64Data);

    // Scanning: discover all VST3 instrument plugins on the system
    static std::vector<VST3PluginInfo> ScanPlugins();

private:
    char name_[80];                         // Instrument name
    char pluginPath_[512];                  // Path to .vst3 bundle
    uint8_t pluginClassId_[16];             // CID of the selected processor class
    int lastNote_[SONG_CHANNEL_COUNT];      // Last note played per channel
    bool playing_[SONG_CHANNEL_COUNT];      // Playing state per channel
    TableSaveState tableState_;

    // Per-channel LFO modulation state
    struct VST3LFOState {
        float phase;
        float speed;
        float depth;
        bool active;
        VST3LFOState() : phase(0), speed(0), depth(0), active(false) {}
    };
    VST3LFOState tremoloLFO_[SONG_CHANNEL_COUNT];
    VST3LFOState vibratoLFO_[SONG_CHANNEL_COUNT];
    VST3LFOState filterLFO_[SONG_CHANNEL_COUNT];   // LFO driving CC74 filter cutoff

    // Per-channel arpeggio state
    struct VST3ArpState {
        unsigned char offsets[5]; // semitone offsets; [0]=root (always 0)
        int length;               // highest active step index (0 = inactive)
        int position;             // current cyclic step
        int baseNote;             // MIDI root note captured at note-start
        bool active;
        VST3ArpState() : length(0), position(0), baseNote(-1), active(false) {
            memset(offsets, 0, sizeof(offsets));
        }
    };
    VST3ArpState arpState_[SONG_CHANNEL_COUNT];

    // Per-channel bit-crush state
    int          crushBits_[SONG_CHANNEL_COUNT];  // 1-16; 16 = bypass
    unsigned char crushDrive_[SONG_CHANNEL_COUNT]; // pre-drive 0-255; 0 = bypass

    // Per-channel reverb state
    int reverbBufferLength_;
    fixed *reverbBuffer_[SONG_CHANNEL_COUNT];
    fixed reverbDecay_[SONG_CHANNEL_COUNT];
    fixed reverbSend_[SONG_CHANNEL_COUNT];
    fixed reverbDamp_[SONG_CHANNEL_COUNT];
    fixed reverbDampL_[SONG_CHANNEL_COUNT];
    fixed reverbDampR_[SONG_CHANNEL_COUNT];
    int reverbPos_[SONG_CHANNEL_COUNT];
    int reverbTapOffsets_[6];

    // VST3 plugin state (opaque pointers — actual types cast in .cpp)
    void *moduleHandle_;                    // dlopen handle
    void *pluginFactory_;                   // IPluginFactory*
    void *component_;                       // IComponent*
    void *audioProcessor_;                  // IAudioProcessor*
    void *editController_;                  // IEditController*
    bool componentIsController_;            // true if component == controller

    // IMidiMapping cache: maps ControllerNumbers → ParamID for CC/pitchbend/aftertouch
    // Populated at load time; used by QueueMidiEvent to send parameter changes
    // instead of kLegacyMIDICCOutEvent when the plugin supports it.
    std::map<int, uint32_t> midiCcToParamId_;

    // Audio buffers
    float *audioBufferL_;
    float *audioBufferR_;
    float *audioInputL_;
    float *audioInputR_;
    int bufferSize_;

    // Mutex protecting pending events
    SysMutex pendingEventsMutex_;

    // Mutex protecting VST3 component state (process vs setState synchronization)
    // Render uses TryLock (never blocks audio thread), loadPresetFromFile uses Lock.
    SysMutex componentMutex_;

    // Set to true when setState times out — the Wine process is stuck and any
    // further IPC calls (setActive, setProcessing, process) would also hang.
    // Once set, Render outputs silence and loadPresetFromFile returns early.
    bool componentDead_;

    // Pending MIDI note events
    struct PendingNoteEvent {
        int16_t channel;
        int16_t pitch;
        float velocity;     // 0.0=note off, >0=note on
        int32_t noteId;
    };
    std::vector<PendingNoteEvent> pendingNoteEvents_;

    // Pending parameter changes to feed to the processor
    struct PendingParamChange {
        uint32_t paramId;
        double value;       // normalized [0,1]
    };
    std::vector<PendingParamChange> pendingParamChanges_;

    // Pending variable values loaded from project file before parameters exist
    std::map<std::string, std::string> pendingParamValues_;

    // Plugin parameters
    std::vector<VST3PluginParameter> parameters_;

    // Program lists / presets (discovered via IUnitInfo or file scanning)
    std::vector<VST3ProgramList> programLists_;
    int currentBank_;                           // index into programLists_
    int currentPreset_;                         // index into current bank's programs
    int32_t programChangeParamIdx_;             // index into parameters_ for kIsProgramChange param (-1 if none)

    // File-based preset support (for plugins that don't expose presets via IUnitInfo)
    bool usingFilePresets_;                     // true when presets come from file scanning
    std::vector<std::vector<VST3FilePreset>> filePresetsByBank_;  // parallel to programLists_

    // Embedded preset support (setState with XML data, like Valhalla/TAL-NoiseMaker)
    bool usingEmbeddedPresets_;                  // true when presets use embedded XML + setState
    std::vector<const char *> embeddedXmlPresets_;  // parallel to programLists_[0].programs
    // Banked embedded XML (Valhalla / TAL style) — programLists_ maps to this
    std::vector<std::vector<const char *>> embeddedXmlByBank_;

    // Embedded compressed XML preset data (for Odin2: zlib-compressed XML → JUCE wrap → setState)
    bool usingCompressedPresets_;
    struct CompressedPreset {
        const uint8_t *data;
        uint32_t compressedSize;
        uint32_t originalSize;
    };
    std::vector<CompressedPreset> compressedPresets_;  // parallel to programLists_[0].programs

    // MIDI-based preset support (for gearmulator/OsTIrus: bank select CC#32 + program change)
    bool usingMidiPresets_;                     // true when presets use MIDI events for selection

    // Pending MIDI CC / program change events to send via kLegacyMIDICCOutEvent
    struct PendingMidiCC {
        uint8_t controlNumber;  // ControllerNumbers enum value
        int8_t  channel;        // MIDI channel 0-15
        int8_t  value;          // 0-127
        int8_t  value2;         // 0-127 (for pitch bend / poly pressure)
    };
    std::vector<PendingMidiCC> pendingMidiCCs_;

    // Render-once-per-cycle caching
    fixed *cachedOutputBuffer_;
    int cachedOutputSize_;
    uint32_t renderChannelMask_;
    Variable *cachedVolVar_;

    // Pre-allocated scratch vectors for audio thread
    std::vector<PendingNoteEvent> renderLocalNotes_;
    std::vector<PendingParamChange> renderLocalParams_;
    std::vector<PendingMidiCC> renderLocalMidiCCs_;

    bool isActive_;
    bool isProcessing_;
    int64_t projectTimeSamples_;  // running sample counter for ProcessContext

    // Helper methods
    void loadPlugin();
    void loadPluginInner();
    void cleanupPlugin();
    void discoverParameters();
    void discoverPresets();
    void discoverPresetFiles();
    void discoverPatchManagerPresets();
    void discoverHardcodedPresets();
    bool loadPresetFromFile(const std::string &filePath, int headerSkipBytes);
    bool loadPresetFromXmlData(const char *xmlData);
    bool loadPresetFromCompressedXmlData(const uint8_t *compressed, uint32_t compSize, uint32_t origSize);
    void setupProcessing(int bufferSize);
};

#endif

#ifndef _LV2_INSTRUMENT_H_
#define _LV2_INSTRUMENT_H_

#include "I_Instrument.h"
#include "Application/Model/Song.h"
#include "System/Process/SysMutex.h"
#include <vector>
#include <string>
#include <cstdint>
#include <map>
#include <lv2/urid/urid.h>

// LV2 instrument parameters
#define LV2IP_PLUGIN        MAKE_FOURCC('P','L','U','G')
#define LV2IP_VOLUME        MAKE_FOURCC('V','O','L','M')
#define LV2IP_PAN           MAKE_FOURCC('P','A','N',' ')
#define LV2IP_TABLE         MAKE_FOURCC('T','A','B','L')
#define LV2IP_TABLEAUTO     MAKE_FOURCC('T','B','L','A')
#define LV2IP_BANK          MAKE_FOURCC('L','B','N','K')
#define LV2IP_PRESET        MAKE_FOURCC('L','P','R','S')

// Info about a single program list (bank) for LV2 preset support
struct LV2ProgramList {
    int32_t listId;                     // Bank index
    std::string name;                   // Bank/category name
    std::vector<std::string> programs;  // Program (preset) names
};

// File-based preset entry for LV2 plugins
struct LV2FilePreset {
    std::string name;       // display name (filename without extension)
    std::string filePath;   // full filesystem path to preset file
};

// Known plugin preset directory mapping for LV2
struct LV2PluginPresetMapping {
    const char *pluginNameSubstring;  // substring to match in plugin name
    const char *directories[6];       // directories to scan (nullptr terminated)
    const char *extension;            // file extension to match
    int headerSkipBytes;              // bytes to skip at start of file
    bool useStateBinary;              // true = urn:juce:stateBinary + atom:Chunk + std base64
                                      // false = <pluginURI>:StateString + JUCE proprietary base64
};

struct LV2ScalePoint {
    float value;
    std::string label;
};

struct LV2PluginParameter {
    std::string name;
    std::string groupName;  // Port group name from TTL
    float minValue;
    float maxValue;
    float defaultValue;
    float currentValue;
    int portIndex;
    Variable *variable;  // Variable for UI binding
    std::vector<LV2ScalePoint> scalePoints;  // Scale points for enumerated values
    bool isEnumeration = false; // True when LV2 port has enumeration property
    bool isOutput = false; // True if port is an output control port (host should not connect writable storage)
    bool isAtomPort = false; // True if the port is an atom port (do not connect float storage)
    std::string resourceURI; // If this parameter is described by an lv2:Parameter resource, store its URI here
};

class LV2Instrument : public I_Instrument, public I_Observer {

public:
    LV2Instrument();
    virtual ~LV2Instrument();

    // I_Observer implementation: notified when WatchedVariables change
    virtual void Update(Observable &o, I_ObservableData *d) override; 

    virtual bool Init();

    // Start & stop the instrument
    virtual bool Start(int channel, unsigned char note, bool retrigger = true);
    virtual void Stop(int channel);

    // Size refers to the number of samples
    // Should always fill interleaved stereo / 16bit
    virtual bool Render(int channel, fixed *buffer, int size, bool updateTick);
    virtual void ProcessCommand(int channel, FourCC cc, ushort value);

    virtual bool IsInitialized();

    virtual bool IsEmpty() { return pluginURI_[0] == '\0'; }

    virtual InstrumentType GetType() { return IT_LV2; }

    virtual const char *GetName();

    virtual void OnStart();

    virtual void Purge();

    virtual int GetTable();
    virtual bool GetTableAutomation();
    virtual void GetTableState(TableSaveState &state);
    virtual void SetTableState(TableSaveState &state);

    // LV2-specific methods
    void SetPlugin(const char *uri);
    const char *GetPluginURI() const { return pluginURI_; }
    
    // Parameter access
    int GetParameterCount() const { return parameters_.size(); }
    const LV2PluginParameter* GetParameter(int index) const {
        if (index >= 0 && index < (int)parameters_.size()) {
            return &parameters_[index];
        }
        return nullptr;
    }
    void SetParameterValue(int index, float value);
    void SetForcedOutputChannels(int count);

    // Scale point support for enumerated parameters
    std::string GetParameterScalePointLabel(int paramIndex, float value) const;
    // Store a variable value read from project file when variable doesn't yet exist
    void StorePendingVariable(const char *name, const char *value);

    // Preset/program support
    int GetBankCount() const { return (int)programLists_.size(); }
    const char *GetBankName(int bankIdx) const;
    int GetCurrentBank() const { return currentBank_; }
    void SetCurrentBank(int bankIdx);
    int GetPresetCount() const;
    int GetPresetCountForBank(int bankIdx) const;
    const char *GetPresetName(int presetIdx) const;
    int GetCurrentPreset() const { return currentPreset_; }
    void SetPreset(int presetIdx);

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

private:
    char name_[80];                     // Instrument name
    char pluginURI_[256];               // LV2 plugin URI
    int lastNote_[SONG_CHANNEL_COUNT];  // Last note played per channel
    bool playing_[SONG_CHANNEL_COUNT];  // Playing state per channel
    TableSaveState tableState_;

    // Per-channel LFO modulation state
    struct LV2LFOState {
        float phase;
        float speed;   // phase increment per sample
        float depth;   // 0..1 normalized
        bool active;
        LV2LFOState() : phase(0), speed(0), depth(0), active(false) {}
    };
    LV2LFOState tremoloLFO_[SONG_CHANNEL_COUNT];
    LV2LFOState vibratoLFO_[SONG_CHANNEL_COUNT];
    
    // Per-channel reverb state (dynamically sized based on sample rate)
    int reverbBufferLength_;  // ~100ms worth of samples at actual sample rate
    fixed *reverbBuffer_[SONG_CHANNEL_COUNT]; // Stereo delay line per channel (heap-allocated)
    fixed reverbDecay_[SONG_CHANNEL_COUNT];   // Decay amount per channel
    fixed reverbSend_[SONG_CHANNEL_COUNT];    // Send level per channel
    fixed reverbDamp_[SONG_CHANNEL_COUNT];    // Damping coefficient per channel
    fixed reverbDampL_[SONG_CHANNEL_COUNT];   // Damping filter state L
    fixed reverbDampR_[SONG_CHANNEL_COUNT];   // Damping filter state R
    int reverbPos_[SONG_CHANNEL_COUNT];       // Write position per channel
    int reverbTapOffsets_[6];                 // Early reflection tap offsets (scaled to sample rate)

    // LV2 plugin state
    void *world_;                       // LilvWorld*
    void *plugin_;                      // const LilvPlugin*
    void *pluginInstance_;              // LilvInstance*
    float *audioBufferL_;               // Left audio buffer (output)
    float *audioBufferR_;               // Right audio buffer (output)
    float *audioInputL_;                // Left input buffer
    float *audioInputR_;                // Right input buffer
    float *audioDummyBuffer_;           // Dummy buffer for extra audio output ports
    int bufferSize_;                    // Current buffer size
    
    // Port indices for audio connections
    int audioInputPortL_;
    int audioInputPortR_;
    int audioOutputPortL_;
    int audioOutputPortR_;
    int midiInputPort_;  // MIDI atom input port
    std::vector<int> audioOutputPorts_; // list of audio output port indices
    int forcedOutputChannels_; // if >0, limit connected output channels to this count
    
    // MIDI event buffer
    uint8_t *midiBuffer_;
    size_t midiBufferSize_;

    // Atom output buffers (for atom:Sequence output ports). Indexed by port index.
    std::vector<uint8_t*> atomOutputBuffers_;
    std::vector<size_t> atomOutputBufferSizes_;

    // Atom input buffers (for atom:Sequence input ports such as MIDI or control atom ports)
    std::vector<uint8_t*> atomInputBuffers_;
    std::vector<size_t> atomInputBufferSizes_;
    
    // Mutex protecting pendingMidiEvents_ and pendingAtomEvents_ which are
    // written from the player thread (Start/Stop/SetParameterValue) and read
    // from the audio thread (Render).
    SysMutex pendingEventsMutex_;

    // Pending MIDI events to write to buffer
    struct MidiEvent {
        uint8_t data[3];
        int size;
    };
    std::vector<MidiEvent> pendingMidiEvents_;
    // Pending variable values loaded from project file before parameters exist
    std::map<std::string,std::string> pendingParamValues_;
    
    // Plugin parameters
    std::vector<LV2PluginParameter> parameters_;
    std::vector<float> controlValues_; // Storage for control port values
    std::vector<float> portControlStorage_; // Per-port storage indexed by port index to safely connect to LV2
    bool isActivated_;  // Track if plugin is activated
    bool portsConnected_; // Track if ports have been connected

    // Render-once-per-cycle: cache the plugin output so multiple channels
    // sharing this instrument don't call lilv_instance_run() multiple times
    fixed *cachedOutputBuffer_;   // Cached fixed-point stereo interleaved output
    int cachedOutputSize_;        // Number of samples in cached output
    uint32_t renderChannelMask_;  // Bitmask of channels rendered this cycle
    Variable *cachedVolVar_;      // Cached pointer to volume variable

    // Pending atom events to write to plugin atom input ports (patch/midi messages etc.)
    struct PendingAtomEvent {
        std::vector<uint8_t> data;
        LV2_URID type;
        int destPortIndex; // destination port index this atom event should be written to
    };
    std::vector<PendingAtomEvent> pendingAtomEvents_;

    // Pre-allocated scratch vectors used inside Render() to avoid heap
    // allocation/deallocation in the audio thread (malloc/free can cause
    // priority inversion → glitches).
    std::vector<MidiEvent> renderLocalMidi_;
    std::vector<PendingAtomEvent> renderLocalAtom_;
    std::vector<PendingAtomEvent> renderPatchEvents_;

    // Preset/program data
    std::vector<LV2ProgramList> programLists_;   // banks/lists of presets
    std::vector<std::vector<LV2FilePreset>> filePresetsByBank_; // file paths for file-based presets
    int currentBank_;
    int currentPreset_;
    bool usingFilePresets_;  // true when presets are loaded from files
    bool usingMidiPresets_;  // true when presets use MIDI bank select + program change

    // Helper methods
    void discoverParameters();
    void loadPlugin();
    void cleanupPlugin();
    void connectPorts(int bufferSize);
    void discoverPresets();
    void discoverPresetFiles();
    void discoverPatchManagerPresets();
    void discoverHardcodedPresets();
    bool loadPresetFromFile(const std::string &filePath, int headerSkipBytes, bool useStateBinary);
    
};

#endif

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

private:
    char name_[80];                     // Instrument name
    char pluginURI_[256];               // LV2 plugin URI
    int lastNote_[SONG_CHANNEL_COUNT];  // Last note played per channel
    bool playing_[SONG_CHANNEL_COUNT];  // Playing state per channel
    TableSaveState tableState_;
    
    // Per-channel reverb state
    static const int LV2_REVERB_BUFFER_LENGTH = 4410; // ~100ms at 44.1kHz
    fixed reverbBuffer_[SONG_CHANNEL_COUNT][LV2_REVERB_BUFFER_LENGTH * 2]; // Stereo delay line per channel
    fixed reverbDecay_[SONG_CHANNEL_COUNT];   // Decay amount per channel
    fixed reverbSend_[SONG_CHANNEL_COUNT];    // Send level per channel
    int reverbPos_[SONG_CHANNEL_COUNT];       // Write position per channel

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
    // Helper methods
    void discoverParameters();
    void loadPlugin();
    void cleanupPlugin();
    void connectPorts(int bufferSize);
    
};

#endif

#ifndef _LV2_INSTRUMENT_H_
#define _LV2_INSTRUMENT_H_

#include "I_Instrument.h"
#include "Application/Model/Song.h"
#include <vector>
#include <string>
#include <cstdint>

// LV2 instrument parameters
#define LV2IP_PLUGIN        MAKE_FOURCC('P','L','U','G')
#define LV2IP_VOLUME        MAKE_FOURCC('V','O','L','M')
#define LV2IP_PAN           MAKE_FOURCC('P','A','N',' ')
#define LV2IP_TABLE         MAKE_FOURCC('T','A','B','L')
#define LV2IP_TABLEAUTO     MAKE_FOURCC('T','B','L','A')

struct LV2PluginParameter {
    std::string name;
    float minValue;
    float maxValue;
    float defaultValue;
    float currentValue;
    int portIndex;
    Variable *variable;  // Variable for UI binding
};

class LV2Instrument : public I_Instrument {

public:
    LV2Instrument();
    virtual ~LV2Instrument();

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

private:
    char name_[80];                     // Instrument name
    char pluginURI_[256];               // LV2 plugin URI
    int lastNote_[SONG_CHANNEL_COUNT];  // Last note played per channel
    bool playing_[SONG_CHANNEL_COUNT];  // Playing state per channel
    TableSaveState tableState_;

    // LV2 plugin state
    void *world_;                       // LilvWorld*
    void *plugin_;                      // const LilvPlugin*
    void *pluginInstance_;              // LilvInstance*
    float *audioBufferL_;               // Left audio buffer (output)
    float *audioBufferR_;               // Right audio buffer (output)
    float *audioInputL_;                // Left input buffer
    float *audioInputR_;                // Right input buffer
    int bufferSize_;                    // Current buffer size
    
    // Port indices for audio connections
    int audioInputPortL_;
    int audioInputPortR_;
    int audioOutputPortL_;
    int audioOutputPortR_;
    int midiInputPort_;  // MIDI atom input port
    
    // MIDI event buffer
    uint8_t *midiBuffer_;
    size_t midiBufferSize_;
    
    // Pending MIDI events to write to buffer
    struct MidiEvent {
        uint8_t data[3];
        int size;
    };
    std::vector<MidiEvent> pendingMidiEvents_;
    
    // Plugin parameters
    std::vector<LV2PluginParameter> parameters_;
    std::vector<float> controlValues_; // Storage for control port values
    bool isActivated_;  // Track if plugin is activated
    
    // Helper methods
    void discoverParameters();
    void loadPlugin();
    void cleanupPlugin();
    void connectPorts(int bufferSize);
};

#endif

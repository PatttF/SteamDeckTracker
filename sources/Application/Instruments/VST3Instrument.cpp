/**
 * VST3Instrument.cpp — headless VST3 plugin hosting for lgpt / SteamDeckTracker
 *
 * Loads a .vst3 bundle via dlopen, creates the processor (IComponent /
 * IAudioProcessor) and optionally the edit controller (IEditController) for
 * parameter discovery.  Audio is processed in 32-bit float blocks and
 * converted to interleaved fixed-point stereo for the tracker engine.
 *
 * MIDI notes are delivered via the VST3 IEventList mechanism.
 * Parameter changes are fed via IParameterChanges.
 *
 * The code avoids any heap allocation in the audio thread (Render).
 */

#include "VST3Instrument.h"
#include "Application/Model/Song.h"
#include "Application/Model/Project.h"
#include "Application/Instruments/CommandList.h"
#include "Services/Audio/Audio.h"
#include "System/Console/Trace.h"
#include "Foundation/Variables/WatchedVariable.h"

#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <dlfcn.h>

// ---- VST3 SDK headers (pluginterfaces only — no compiled library needed) ----
// IID definitions are provided by VST3IIDs.cpp (VST interfaces) and
// coreiids.cpp (base interfaces).  funknown.cpp provides the FUID class.
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/vsttypes.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstunits.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

// ---- Module entry/exit function typedefs (Linux) ----
extern "C" {
typedef bool (*VST3ModuleEntryFunc)(void*);
typedef bool (*VST3ModuleExitFunc)();
typedef IPluginFactory* (*VST3GetFactoryFunc)();
}

// ====================================================================
// Lightweight IHostApplication implementation (required by many plugins)
// ====================================================================
class SimpleHostApplication : public IHostApplication {
public:
    virtual ~SimpleHostApplication() {}
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(_iid, IHostApplication::iid) ||
            FUnknownPrivate::iidEqual(_iid, FUnknown::iid)) {
            *obj = static_cast<IHostApplication*>(this);
            addRef();
            return kResultTrue;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refCount_; }
    uint32 PLUGIN_API release() override {
        if (--refCount_ == 0) { delete this; return 0; }
        return refCount_;
    }
    tresult PLUGIN_API getName(String128 name) override {
        const char16_t n[] = u"SteamDeckTracker";
        memcpy(name, n, sizeof(n));
        return kResultTrue;
    }
    tresult PLUGIN_API createInstance(TUID /*cid*/, TUID /*_iid*/, void** obj) override {
        *obj = nullptr;
        return kNotImplemented;
    }
private:
    int32_t refCount_ = 1;
};

static SimpleHostApplication gHostApp;

// ====================================================================
// Lightweight IComponentHandler (required by IEditController::setComponentHandler)
// ====================================================================
class SimpleComponentHandler : public IComponentHandler {
public:
    virtual ~SimpleComponentHandler() {}
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(_iid, IComponentHandler::iid) ||
            FUnknownPrivate::iidEqual(_iid, FUnknown::iid)) {
            *obj = static_cast<IComponentHandler*>(this);
            addRef();
            return kResultTrue;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refCount_; }
    uint32 PLUGIN_API release() override {
        if (--refCount_ == 0) { delete this; return 0; }
        return refCount_;
    }
    tresult PLUGIN_API beginEdit(ParamID /*id*/) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID /*id*/, ParamValue /*valueNormalized*/) override { return kResultOk; }
    tresult PLUGIN_API endEdit(ParamID /*id*/) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32 /*flags*/) override { return kResultOk; }
private:
    int32_t refCount_ = 1;
};

// ====================================================================
// Lightweight EventList for sending note events to the processor
// ====================================================================
class SimpleEventList : public IEventList {
public:
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(_iid, IEventList::iid) ||
            FUnknownPrivate::iidEqual(_iid, FUnknown::iid)) {
            *obj = static_cast<IEventList*>(this);
            addRef();
            return kResultTrue;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; } // stack-allocated
    uint32 PLUGIN_API release() override { return 1; }

    int32 PLUGIN_API getEventCount() override { return (int32)events_.size(); }
    tresult PLUGIN_API getEvent(int32 index, Event& e) override {
        if (index < 0 || index >= (int32)events_.size()) return kInvalidArgument;
        e = events_[index];
        return kResultOk;
    }
    tresult PLUGIN_API addEvent(Event& e) override {
        events_.push_back(e);
        return kResultOk;
    }
    void clear() { events_.clear(); }
private:
    std::vector<Event> events_;
};

// ====================================================================
// Lightweight IParamValueQueue
// ====================================================================
class SimpleParamValueQueue : public IParamValueQueue {
public:
    SimpleParamValueQueue() : paramId_(0) {}
    void setParamId(ParamID id) { paramId_ = id; }

    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(_iid, IParamValueQueue::iid) ||
            FUnknownPrivate::iidEqual(_iid, FUnknown::iid)) {
            *obj = static_cast<IParamValueQueue*>(this);
            return kResultTrue;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }

    ParamID PLUGIN_API getParameterId() override { return paramId_; }
    int32 PLUGIN_API getPointCount() override { return (int32)points_.size(); }
    tresult PLUGIN_API getPoint(int32 index, int32& sampleOffset, ParamValue& value) override {
        if (index < 0 || index >= (int32)points_.size()) return kInvalidArgument;
        sampleOffset = points_[index].first;
        value = points_[index].second;
        return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32 sampleOffset, ParamValue value, int32& index) override {
        index = (int32)points_.size();
        points_.push_back(std::make_pair(sampleOffset, value));
        return kResultOk;
    }
    void clear() { points_.clear(); }
private:
    ParamID paramId_;
    std::vector<std::pair<int32, ParamValue>> points_;
};

// ====================================================================
// Lightweight IParameterChanges
// ====================================================================
class SimpleParameterChanges : public IParameterChanges {
public:
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(_iid, IParameterChanges::iid) ||
            FUnknownPrivate::iidEqual(_iid, FUnknown::iid)) {
            *obj = static_cast<IParameterChanges*>(this);
            return kResultTrue;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }

    int32 PLUGIN_API getParameterCount() override { return (int32)queues_.size(); }
    IParamValueQueue* PLUGIN_API getParameterData(int32 index) override {
        if (index < 0 || index >= (int32)queues_.size()) return nullptr;
        return &queues_[index];
    }
    IParamValueQueue* PLUGIN_API addParameterData(const ParamID& id, int32& index) override {
        // Check if we already have a queue for this param
        for (int32 i = 0; i < (int32)queues_.size(); i++) {
            if (queues_[i].getParameterId() == id) {
                index = i;
                return &queues_[i];
            }
        }
        index = (int32)queues_.size();
        queues_.push_back(SimpleParamValueQueue());
        queues_.back().setParamId(id);
        return &queues_.back();
    }
    void clear() {
        for (auto& q : queues_) q.clear();
        queues_.clear();
    }
private:
    std::vector<SimpleParamValueQueue> queues_;
};

// ====================================================================
// Lightweight IBStream implementation (in-memory)
// Used to transfer component state to a separate edit controller.
// ====================================================================
class SimpleMemoryStream : public IBStream {
public:
    SimpleMemoryStream() : pos_(0), refCount_(1) {}
    virtual ~SimpleMemoryStream() {}

    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(_iid, IBStream::iid) ||
            FUnknownPrivate::iidEqual(_iid, FUnknown::iid)) {
            *obj = static_cast<IBStream*>(this);
            addRef();
            return kResultTrue;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refCount_; }
    uint32 PLUGIN_API release() override {
        if (--refCount_ == 0) { delete this; return 0; }
        return refCount_;
    }

    tresult PLUGIN_API read(void* buffer, int32 numBytes, int32* numBytesRead) override {
        if (!buffer || numBytes < 0) return kInvalidArgument;
        int32 avail = (int32)data_.size() - (int32)pos_;
        if (avail < 0) avail = 0;
        int32 toRead = (numBytes < avail) ? numBytes : avail;
        if (toRead > 0) {
            memcpy(buffer, &data_[pos_], toRead);
            pos_ += toRead;
        }
        if (numBytesRead) *numBytesRead = toRead;
        return kResultOk;
    }

    tresult PLUGIN_API write(void* buffer, int32 numBytes, int32* numBytesWritten) override {
        if (!buffer || numBytes < 0) return kInvalidArgument;
        size_t end = pos_ + numBytes;
        if (end > data_.size()) data_.resize(end);
        memcpy(&data_[pos_], buffer, numBytes);
        pos_ += numBytes;
        if (numBytesWritten) *numBytesWritten = numBytes;
        return kResultOk;
    }

    tresult PLUGIN_API seek(int64 pos, int32 mode, int64* result) override {
        int64 newPos = 0;
        switch (mode) {
            case kIBSeekSet: newPos = pos; break;
            case kIBSeekCur: newPos = (int64)pos_ + pos; break;
            case kIBSeekEnd: newPos = (int64)data_.size() + pos; break;
            default: return kInvalidArgument;
        }
        if (newPos < 0) newPos = 0;
        pos_ = (size_t)newPos;
        if (result) *result = (int64)pos_;
        return kResultOk;
    }

    tresult PLUGIN_API tell(int64* pos) override {
        if (!pos) return kInvalidArgument;
        *pos = (int64)pos_;
        return kResultOk;
    }

    void resetRead() { pos_ = 0; }
    size_t size() const { return data_.size(); }

private:
    std::vector<uint8_t> data_;
    size_t pos_;
    int32_t refCount_;
};

// ====================================================================
// Helpers: get machine arch string for .vst3 bundle Contents/<arch>-linux/
// ====================================================================
static std::string getLinuxArchString() {
    struct utsname uts;
    if (uname(&uts) == 0) {
        return std::string(uts.machine) + "-linux";
    }
    return "x86_64-linux";
}

// Resolve the .so path inside a .vst3 bundle
static std::string resolveVST3SOPath(const std::string& bundlePath) {
    std::string arch = getLinuxArchString();
    // bundlePath e.g. /usr/lib/vst3/Vital.vst3
    // stem = Vital.vst3 -> Vital
    std::string stem = bundlePath;
    size_t lastSlash = stem.rfind('/');
    if (lastSlash != std::string::npos) stem = stem.substr(lastSlash + 1);
    // Remove .vst3 extension
    size_t extPos = stem.rfind(".vst3");
    if (extPos != std::string::npos) stem = stem.substr(0, extPos);

    std::string soPath = bundlePath + "/Contents/" + arch + "/" + stem + ".so";
    struct stat st;
    if (stat(soPath.c_str(), &st) == 0) return soPath;
    return "";
}

// Recursively find .vst3 bundles in a directory
static void findVST3Bundles(const std::string& dir, std::vector<std::string>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        std::string name(ent->d_name);
        std::string full = dir + "/" + name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (name.size() > 5 && name.substr(name.size() - 4) == "vst3") {
                // Check it ends with .vst3
                if (name.size() > 4 && name[name.size()-5] == '.') {
                    out.push_back(full);
                }
            } else {
                findVST3Bundles(full, out);
            }
        }
    }
    closedir(d);
}

// Convert TUID (16 bytes) to hex string
static std::string tuidToHex(const uint8_t* tuid) {
    char buf[33];
    for (int i = 0; i < 16; i++) {
        sprintf(buf + i * 2, "%02X", tuid[i]);
    }
    buf[32] = '\0';
    return std::string(buf);
}

// Convert hex string to TUID
static void hexToTuid(const std::string& hex, uint8_t out[16]) {
    memset(out, 0, 16);
    for (int i = 0; i < 16 && i * 2 + 1 < (int)hex.size(); i++) {
        unsigned int val;
        sscanf(hex.c_str() + i * 2, "%02X", &val);
        out[i] = (uint8_t)val;
    }
}

// Convert char16_t String128 to C string (ASCII only)
static void string128ToChar(const TChar* src, char* dst, int maxLen) {
    int i = 0;
    while (i < maxLen - 1 && src[i]) {
        dst[i] = (char)(src[i] & 0x7F);
        i++;
    }
    dst[i] = '\0';
}

// ====================================================================
// VST3Instrument implementation
// ====================================================================

VST3Instrument::VST3Instrument() {
    strcpy(name_, "VST3");
    memset(pluginPath_, 0, sizeof(pluginPath_));
    memset(pluginClassId_, 0, sizeof(pluginClassId_));

    for (int i = 0; i < SONG_CHANNEL_COUNT; i++) {
        lastNote_[i] = -1;
        playing_[i] = false;
        tremoloLFO_[i] = VST3LFOState();
        vibratoLFO_[i] = VST3LFOState();
        reverbBuffer_[i] = nullptr;
        reverbDecay_[i] = 0;
        reverbSend_[i] = 0;
        reverbDamp_[i] = 0;
        reverbDampL_[i] = 0;
        reverbDampR_[i] = 0;
        reverbPos_[i] = 0;
    }

    moduleHandle_ = nullptr;
    pluginFactory_ = nullptr;
    component_ = nullptr;
    audioProcessor_ = nullptr;
    editController_ = nullptr;
    componentIsController_ = false;

    audioBufferL_ = nullptr;
    audioBufferR_ = nullptr;
    audioInputL_ = nullptr;
    audioInputR_ = nullptr;
    bufferSize_ = 0;

    cachedOutputBuffer_ = nullptr;
    cachedOutputSize_ = 0;
    renderChannelMask_ = 0;
    cachedVolVar_ = nullptr;

    isActive_ = false;
    isProcessing_ = false;

    currentBank_ = 0;
    currentPreset_ = 0;
    programChangeParamIdx_ = -1;

    // Compute reverb buffer length based on sample rate (~100ms)
    int sampleRate = Audio::GetInstance()->GetSampleRate();
    if (sampleRate <= 0) sampleRate = 44100;
    reverbBufferLength_ = (sampleRate / 10);
    if (reverbBufferLength_ < 4410) reverbBufferLength_ = 4410;

    // Allocate reverb buffers
    for (int i = 0; i < SONG_CHANNEL_COUNT; i++) {
        reverbBuffer_[i] = (fixed*)malloc(reverbBufferLength_ * 2 * sizeof(fixed));
        if (reverbBuffer_[i]) {
            memset(reverbBuffer_[i], 0, reverbBufferLength_ * 2 * sizeof(fixed));
        }
    }

    // Set up reverb tap offsets
    const float tapTimes[] = {0.029f, 0.037f, 0.041f, 0.053f, 0.067f, 0.073f};
    for (int i = 0; i < 6; i++) {
        reverbTapOffsets_[i] = (int)(tapTimes[i] * sampleRate);
        if (reverbTapOffsets_[i] >= reverbBufferLength_)
            reverbTapOffsets_[i] = reverbBufferLength_ - 1;
    }

    // Create standard variables
    Variable *v;
    v = new Variable("plugin", VST3IP_PLUGIN, "");
    Insert(v);
    v = new Variable("volume", VST3IP_VOLUME, 255);
    Insert(v);
    v = new Variable("pan", VST3IP_PAN, 0x7F);
    Insert(v);
    v = new Variable("table", VST3IP_TABLE, -1);
    Insert(v);
    v = new Variable("table automation", VST3IP_TABLEAUTO, false);
    Insert(v);
}

VST3Instrument::~VST3Instrument() {
    // Remove observers from watched variables
    for (size_t i = 0; i < parameters_.size(); ++i) {
        if (parameters_[i].variable) {
            WatchedVariable *wv = dynamic_cast<WatchedVariable*>(parameters_[i].variable);
            if (wv) wv->RemoveObserver(*this);
        }
    }
    cleanupPlugin();
    for (int i = 0; i < SONG_CHANNEL_COUNT; i++) {
        free(reverbBuffer_[i]);
        reverbBuffer_[i] = nullptr;
    }
    free(cachedOutputBuffer_);
    cachedOutputBuffer_ = nullptr;
}

bool VST3Instrument::Init() {
    // Reload plugin from saved path if we have one
    Variable *plugVar = FindVariable(VST3IP_PLUGIN);
    if (plugVar) {
        const char *path = plugVar->GetString();
        if (path && path[0] != '\0') {
            // Path is stored as "path|classIdHex"
            std::string combined(path);
            size_t sep = combined.find('|');
            if (sep != std::string::npos) {
                std::string p = combined.substr(0, sep);
                std::string cidHex = combined.substr(sep + 1);
                uint8_t cid[16];
                hexToTuid(cidHex, cid);
                SetPlugin(p.c_str(), cid);

                // Apply pending parameter values that were loaded from project
                for (auto& kv : pendingParamValues_) {
                    Variable *var = FindVariable(kv.first.c_str());
                    if (var) {
                        var->SetString(kv.second.c_str());
                    }
                }
                pendingParamValues_.clear();
            }
        }
    }
    return true;
}

void VST3Instrument::OnStart() {
    renderChannelMask_ = 0;
}

bool VST3Instrument::Start(int channel, unsigned char note, bool retrigger) {
    if (channel < 0 || channel >= SONG_CHANNEL_COUNT) return false;
    if (!audioProcessor_) return false;

    // Queue note on
    PendingNoteEvent evt;
    evt.channel = 0; // VST3 always channel 0
    evt.pitch = note;
    evt.velocity = 0.8f;
    evt.noteId = note; // use pitch as noteId

    pendingEventsMutex_.Lock();
    // If retriggering, send note-off for previous note
    if (retrigger && playing_[channel] && lastNote_[channel] >= 0) {
        PendingNoteEvent offEvt;
        offEvt.channel = 0;
        offEvt.pitch = lastNote_[channel];
        offEvt.velocity = 0.0f;
        offEvt.noteId = lastNote_[channel];
        pendingNoteEvents_.push_back(offEvt);
    }
    pendingNoteEvents_.push_back(evt);
    pendingEventsMutex_.Unlock();

    lastNote_[channel] = note;
    playing_[channel] = true;

    // Reset LFO phases on new note
    tremoloLFO_[channel].phase = 0;
    vibratoLFO_[channel].phase = 0;

    return true;
}

void VST3Instrument::Stop(int channel) {
    if (channel < 0 || channel >= SONG_CHANNEL_COUNT) return;
    if (!playing_[channel]) return;

    // Queue note off
    PendingNoteEvent evt;
    evt.channel = 0;
    evt.pitch = lastNote_[channel];
    evt.velocity = 0.0f;
    evt.noteId = lastNote_[channel];

    pendingEventsMutex_.Lock();
    pendingNoteEvents_.push_back(evt);
    pendingEventsMutex_.Unlock();

    playing_[channel] = false;
}

bool VST3Instrument::Render(int channel, fixed *buffer, int size, bool updateTick) {
    if (channel < 0 || channel >= SONG_CHANNEL_COUNT) {
        memset(buffer, 0, size * 2 * sizeof(fixed));
        return false;
    }

    bool noteActive = playing_[channel];

    // Render-once-per-cycle: detect new audio cycle when the same channel
    // bit is already set (meaning we've come around again).
    uint32_t channelBit = (1u << channel);
    if (renderChannelMask_ & channelBit) {
        renderChannelMask_ = 0;  // new cycle
    }

    bool needsPluginRun = (renderChannelMask_ == 0);
    renderChannelMask_ |= channelBit;

    if (needsPluginRun && audioProcessor_) {
        IAudioProcessor* proc = (IAudioProcessor*)audioProcessor_;
        IComponent* comp = (IComponent*)component_;

        // Allocate buffers on first use or resize
        if (size > bufferSize_) {
            free(audioBufferL_);
            free(audioBufferR_);
            free(audioInputL_);
            free(audioInputR_);
            audioBufferL_ = (float*)calloc(size, sizeof(float));
            audioBufferR_ = (float*)calloc(size, sizeof(float));
            audioInputL_ = (float*)calloc(size, sizeof(float));
            audioInputR_ = (float*)calloc(size, sizeof(float));
            bufferSize_ = size;

            free(cachedOutputBuffer_);
            cachedOutputBuffer_ = (fixed*)calloc(size * 2, sizeof(fixed));
            cachedOutputSize_ = size;

            // Re-setup processing with new block size
            if (isProcessing_) {
                proc->setProcessing(false);
                isProcessing_ = false;
            }
            if (isActive_) {
                comp->setActive(false);
                isActive_ = false;
            }
            setupProcessing(size);
        }

        // Build event list and parameter changes (copy pending events under lock)
        SimpleEventList eventList;
        SimpleParameterChanges paramChanges;

        if (pendingEventsMutex_.TryLock()) {
            renderLocalNotes_.swap(pendingNoteEvents_);
            renderLocalParams_.swap(pendingParamChanges_);
            pendingEventsMutex_.Unlock();
        }

        // Sync parameter values from Variables to parameter changes
        // (runs on audio thread only, so no lock needed for the variables)
        for (size_t i = 0; i < parameters_.size(); ++i) {
            VST3PluginParameter& p = parameters_[i];
            if (p.variable && !p.isReadOnly) {
                int varVal = p.variable->GetInt();
                int maxVal = (p.stepCount > 0 && p.stepCount <= 255) ? p.stepCount : 255;
                double norm = (double)varVal / (double)maxVal;
                if (norm < 0.0) norm = 0.0;
                if (norm > 1.0) norm = 1.0;

                // Only queue change if value actually changed
                if (fabs(norm - p.currentValue) > 0.0001) {
                    p.currentValue = norm;
                    int32 idx;
                    IParamValueQueue* queue = paramChanges.addParameterData(p.paramId, idx);
                    if (queue) {
                        int32 ptIdx;
                        queue->addPoint(0, norm, ptIdx);
                    }
                }
            }
        }

        // Convert pending notes to VST3 Events
        for (auto& ne : renderLocalNotes_) {
            Event e;
            memset(&e, 0, sizeof(e));
            e.busIndex = 0;
            e.sampleOffset = 0;

            if (ne.velocity > 0.0f) {
                e.type = Event::kNoteOnEvent;
                e.noteOn.channel = ne.channel;
                e.noteOn.pitch = ne.pitch;
                e.noteOn.velocity = ne.velocity;
                e.noteOn.noteId = ne.noteId;
                e.noteOn.tuning = 0.0f;
                e.noteOn.length = 0;
            } else {
                e.type = Event::kNoteOffEvent;
                e.noteOff.channel = ne.channel;
                e.noteOff.pitch = ne.pitch;
                e.noteOff.velocity = 0.0f;
                e.noteOff.noteId = ne.noteId;
                e.noteOff.tuning = 0.0f;
            }
            eventList.addEvent(e);
        }
        renderLocalNotes_.clear();

        // Convert pending param changes
        for (auto& pc : renderLocalParams_) {
            int32 idx;
            IParamValueQueue* queue = paramChanges.addParameterData(pc.paramId, idx);
            if (queue) {
                int32 ptIdx;
                queue->addPoint(0, pc.value, ptIdx);
            }
        }
        renderLocalParams_.clear();

        // Clear audio buffers
        memset(audioBufferL_, 0, size * sizeof(float));
        memset(audioBufferR_, 0, size * sizeof(float));
        memset(audioInputL_, 0, size * sizeof(float));
        memset(audioInputR_, 0, size * sizeof(float));

        // Set up ProcessData
        ProcessData processData;
        processData.processMode = kRealtime;
        processData.symbolicSampleSize = kSample32;
        processData.numSamples = size;

        // Input audio bus
        AudioBusBuffers inputBus;
        float* inputChannels[2] = { audioInputL_, audioInputR_ };
        inputBus.numChannels = 2;
        inputBus.channelBuffers32 = inputChannels;
        inputBus.silenceFlags = 3; // both channels silent

        // Output audio bus
        AudioBusBuffers outputBus;
        float* outputChannels[2] = { audioBufferL_, audioBufferR_ };
        outputBus.numChannels = 2;
        outputBus.channelBuffers32 = outputChannels;
        outputBus.silenceFlags = 0;

        // Check bus counts: some instruments have no input audio bus
        int32 numInputBuses = comp->getBusCount(kAudio, kInput);
        int32 numOutputBuses = comp->getBusCount(kAudio, kOutput);

        if (numInputBuses > 0) {
            processData.numInputs = 1;
            processData.inputs = &inputBus;
        } else {
            processData.numInputs = 0;
            processData.inputs = nullptr;
        }
        if (numOutputBuses > 0) {
            processData.numOutputs = 1;
            processData.outputs = &outputBus;
        } else {
            processData.numOutputs = 0;
            processData.outputs = nullptr;
        }

        processData.inputEvents = &eventList;
        processData.inputParameterChanges = &paramChanges;
        processData.outputParameterChanges = nullptr;
        processData.outputEvents = nullptr;
        processData.processContext = nullptr;

        // Run the plugin
        processData.numSamples = size;
        proc->process(processData);

        // Sanitise NaN/Inf
        {
            union { float f; uint32_t u; } chk;
            for (int i = 0; i < size; i++) {
                chk.f = audioBufferL_[i];
                if ((chk.u & 0x7F800000u) == 0x7F800000u) audioBufferL_[i] = 0.0f;
                chk.f = audioBufferR_[i];
                if ((chk.u & 0x7F800000u) == 0x7F800000u) audioBufferR_[i] = 0.0f;
            }
        }

        // Apply volume and convert to fixed-point
        if (!cachedVolVar_) cachedVolVar_ = FindVariable(VST3IP_VOLUME);
        float volume = cachedVolVar_ ? (cachedVolVar_->GetInt() / 255.0f) : 1.0f;

        for (int i = 0; i < size; i++) {
            float l = audioBufferL_[i] * volume;
            float r = audioBufferR_[i] * volume;

            // Bitwise NaN/Inf check
            union { float f; uint32_t u; } ul, ur;
            ul.f = l; ur.f = r;
            if ((ul.u & 0x7F800000u) == 0x7F800000u) l = 0.0f;
            if ((ur.u & 0x7F800000u) == 0x7F800000u) r = 0.0f;

            if (l > 1.0f) l = 1.0f;
            if (l < -1.0f) l = -1.0f;
            if (r > 1.0f) r = 1.0f;
            if (r < -1.0f) r = -1.0f;

            cachedOutputBuffer_[i * 2] = i2fp((int)(l * 32767.0f));
            cachedOutputBuffer_[i * 2 + 1] = i2fp((int)(r * 32767.0f));
        }
    } // end needsPluginRun

    // Copy cached output to this channel's buffer
    if (cachedOutputBuffer_ && cachedOutputSize_ >= size) {
        memcpy(buffer, cachedOutputBuffer_, size * 2 * sizeof(fixed));
    } else {
        memset(buffer, 0, size * 2 * sizeof(fixed));
    }

    // Apply per-channel tremolo
    if (tremoloLFO_[channel].active) {
        float phase = tremoloLFO_[channel].phase;
        float speed = tremoloLFO_[channel].speed;
        float depth = tremoloLFO_[channel].depth;
        for (int i = 0; i < size; i++) {
            float sine = sinf(phase * 2.0f * 3.14159265f / 256.0f);
            float volMul = 1.0f + sine * depth;
            if (volMul < 0.0f) volMul = 0.0f;
            fixed mul = fl2fp(volMul);
            buffer[i * 2]     = fp_mul(buffer[i * 2], mul);
            buffer[i * 2 + 1] = fp_mul(buffer[i * 2 + 1], mul);
            phase += speed;
            if (phase >= 256.0f) phase -= 256.0f;
        }
        tremoloLFO_[channel].phase = phase;
    }

    // Apply per-channel reverb
    if (reverbSend_[channel] > 0 && reverbBuffer_[channel]) {
        fixed *reverbBuf = reverbBuffer_[channel];
        int reverbPos = reverbPos_[channel];
        fixed decay = reverbDecay_[channel];
        fixed send = reverbSend_[channel];
        fixed damp = reverbDamp_[channel];
        fixed dampInv = fp_sub(FP_ONE, damp);
        fixed dampStateL = reverbDampL_[channel];
        fixed dampStateR = reverbDampR_[channel];

        static const fixed tapGains[6] = {
            fl2fp(0.25f), fl2fp(0.30f), fl2fp(0.20f),
            fl2fp(0.18f), fl2fp(0.14f), fl2fp(0.10f)
        };
        const int fbSamples = (int)(100.0f * (float)reverbBufferLength_ / 4410.0f);

        fixed *outPtr = buffer;
        for (int i = 0; i < size; i++) {
            fixed dryL = *outPtr;
            fixed dryR = *(outPtr + 1);

            fixed wetL = 0, wetR = 0;
            for (int t = 0; t < 6; t++) {
                int readPos = reverbPos - reverbTapOffsets_[t];
                if (readPos < 0) readPos += reverbBufferLength_;
                fixed tapL = fp_mul(reverbBuf[readPos * 2], tapGains[t]);
                fixed tapR = fp_mul(reverbBuf[readPos * 2 + 1], tapGains[t]);
                if (t & 1) { wetL = fp_add(wetL, tapR); wetR = fp_add(wetR, tapL); }
                else       { wetL = fp_add(wetL, tapL); wetR = fp_add(wetR, tapR); }
            }

            int fb_pos = reverbPos - reverbBufferLength_ + fbSamples;
            if (fb_pos < 0) fb_pos += reverbBufferLength_;
            fixed rawFbL = reverbBuf[fb_pos * 2];
            fixed rawFbR = reverbBuf[fb_pos * 2 + 1];
            dampStateL = fp_add(fp_mul(rawFbL, dampInv), fp_mul(dampStateL, damp));
            dampStateR = fp_add(fp_mul(rawFbR, dampInv), fp_mul(dampStateR, damp));
            fixed fbL = fp_mul(dampStateL, decay);
            fixed fbR = fp_mul(dampStateR, decay);

            reverbBuf[reverbPos * 2] = fp_add(fp_mul(dryL, send), fbL);
            reverbBuf[reverbPos * 2 + 1] = fp_add(fp_mul(dryR, send), fbR);

            fixed mixL = fp_add(dryL, wetL);
            fixed mixR = fp_add(dryR, wetR);
            if (mixL > i2fp(32767)) mixL = i2fp(32767);
            if (mixL < i2fp(-32768)) mixL = i2fp(-32768);
            if (mixR > i2fp(32767)) mixR = i2fp(32767);
            if (mixR < i2fp(-32768)) mixR = i2fp(-32768);
            *outPtr = mixL;
            *(outPtr + 1) = mixR;

            outPtr += 2;
            reverbPos++;
            if (reverbPos >= reverbBufferLength_) reverbPos = 0;
        }
        reverbPos_[channel] = reverbPos;
        reverbDampL_[channel] = dampStateL;
        reverbDampR_[channel] = dampStateR;
    }

    // Check for silence on finished notes (release tail)
    if (!noteActive) {
        bool isSilent = true;
        const fixed silenceThreshold = i2fp(8);
        for (int i = 0; i < size * 2; i++) {
            fixed s = buffer[i];
            if (s > silenceThreshold || s < -silenceThreshold) {
                isSilent = false;
                break;
            }
        }
        return !isSilent;
    }

    return true;
}

void VST3Instrument::ProcessCommand(int channel, FourCC cc, ushort value) {
    if (channel < 0 || channel >= SONG_CHANNEL_COUNT) return;

    if (cc == I_CMD_REVB) {
        unsigned char decayNibble = (value >> 12) & 0x0F;
        unsigned char dampNibble = (value >> 8) & 0x0F;
        unsigned char sendAmount = value & 0xFF;
        reverbDecay_[channel] = fl2fp((decayNibble / 15.0f) * 0.9f);
        reverbDamp_[channel] = fl2fp((dampNibble / 15.0f) * 0.85f);
        reverbSend_[channel] = fl2fp(sendAmount / 255.0f);
    } else if (cc == I_CMD_TRML) {
        unsigned char speed = (value >> 8) & 0xFF;
        unsigned char depth = value & 0xFF;
        tremoloLFO_[channel].speed = (speed * 2.0f) / 100.0f;
        tremoloLFO_[channel].depth = depth / 255.0f;
        tremoloLFO_[channel].active = true;
    } else if (cc == I_CMD_VIBR) {
        unsigned char speed = (value >> 8) & 0xFF;
        unsigned char depth = value & 0xFF;
        vibratoLFO_[channel].speed = (speed * 2.0f) / 100.0f;
        vibratoLFO_[channel].depth = depth / 255.0f;
        vibratoLFO_[channel].active = true;
    }
}

bool VST3Instrument::IsInitialized() {
    return true;
}

const char *VST3Instrument::GetName() {
    if (IsEmpty()) return "-- no plugin --";
    return name_;
}

void VST3Instrument::Purge() {
    for (size_t i = 0; i < parameters_.size(); ++i) {
        if (parameters_[i].variable) {
            WatchedVariable *wv = dynamic_cast<WatchedVariable*>(parameters_[i].variable);
            if (wv) wv->RemoveObserver(*this);
        }
    }
    cleanupPlugin();
    pluginPath_[0] = '\0';
    memset(pluginClassId_, 0, sizeof(pluginClassId_));
    strcpy(name_, "VST3");
    parameters_.clear();
}

int VST3Instrument::GetTable() {
    Variable *v = FindVariable(VST3IP_TABLE);
    return v ? v->GetInt() : -1;
}

bool VST3Instrument::GetTableAutomation() {
    Variable *v = FindVariable(VST3IP_TABLEAUTO);
    return v ? v->GetBool() : false;
}

void VST3Instrument::GetTableState(TableSaveState &state) {
    state = tableState_;
}

void VST3Instrument::SetTableState(TableSaveState &state) {
    tableState_ = state;
}

void VST3Instrument::Update(Observable &o, I_ObservableData *d) {
    // No-op: audio thread reads Variables directly each cycle
}

void VST3Instrument::StorePendingVariable(const char *name, const char *value) {
    pendingParamValues_[std::string(name)] = std::string(value);
}

// ====================================================================
// SetPlugin: load a .vst3 bundle and discover parameters
// ====================================================================
void VST3Instrument::SetPlugin(const char *path, const uint8_t classId[16]) {
    // Cleanup any existing plugin
    cleanupPlugin();

    strncpy(pluginPath_, path, sizeof(pluginPath_) - 1);
    pluginPath_[sizeof(pluginPath_) - 1] = '\0';
    memcpy(pluginClassId_, classId, 16);

    // Store combined path|classId in the VPLG variable
    Variable *plugVar = FindVariable(VST3IP_PLUGIN);
    if (plugVar) {
        std::string combined = std::string(pluginPath_) + "|" + tuidToHex(pluginClassId_);
        plugVar->SetString(combined.c_str());
    }

    loadPlugin();
}

void VST3Instrument::SetParameterValue(int index, double normalizedValue) {
    if (index < 0 || index >= (int)parameters_.size()) return;

    VST3PluginParameter& p = parameters_[index];
    p.currentValue = normalizedValue;

    // Queue a parameter change for the audio thread
    PendingParamChange pc;
    pc.paramId = p.paramId;
    pc.value = normalizedValue;

    pendingEventsMutex_.Lock();
    pendingParamChanges_.push_back(pc);
    pendingEventsMutex_.Unlock();
}

// ====================================================================
// GetParameterDisplayString: ask the controller for a display label
// ====================================================================
std::string VST3Instrument::GetParameterDisplayString(int index, double normalizedValue) const {
    if (!editController_) return "";
    if (index < 0 || index >= (int)parameters_.size()) return "";

    IEditController* ctrl = (IEditController*)editController_;
    const VST3PluginParameter& p = parameters_[index];

    String128 display;
    tresult res = ctrl->getParamStringByValue(p.paramId, normalizedValue, display);
    if (res == kResultOk) {
        char buf[128];
        string128ToChar(display, buf, 128);
        return std::string(buf);
    }
    return "";
}

// ====================================================================
// loadPlugin: dlopen the .vst3 bundle, create component + controller
// ====================================================================
void VST3Instrument::loadPlugin() {
    if (pluginPath_[0] == '\0') return;

    std::string soPath = resolveVST3SOPath(pluginPath_);
    if (soPath.empty()) {
        Trace::Error("VST3: Could not resolve .so path for %s", pluginPath_);
        return;
    }

    // dlopen the module
    moduleHandle_ = dlopen(soPath.c_str(), RTLD_LAZY);
    if (!moduleHandle_) {
        Trace::Error("VST3: dlopen failed: %s", dlerror());
        return;
    }

    // Call ModuleEntry (mandatory on Linux)
    VST3ModuleEntryFunc moduleEntry = (VST3ModuleEntryFunc)dlsym(moduleHandle_, "ModuleEntry");
    if (!moduleEntry) {
        Trace::Error("VST3: Module does not export ModuleEntry");
        dlclose(moduleHandle_);
        moduleHandle_ = nullptr;
        return;
    }
    if (!moduleEntry(moduleHandle_)) {
        Trace::Error("VST3: ModuleEntry failed");
        dlclose(moduleHandle_);
        moduleHandle_ = nullptr;
        return;
    }

    // Get plugin factory
    VST3GetFactoryFunc getFactory = (VST3GetFactoryFunc)dlsym(moduleHandle_, "GetPluginFactory");
    if (!getFactory) {
        Trace::Error("VST3: Module does not export GetPluginFactory");
        VST3ModuleExitFunc moduleExit = (VST3ModuleExitFunc)dlsym(moduleHandle_, "ModuleExit");
        if (moduleExit) moduleExit();
        dlclose(moduleHandle_);
        moduleHandle_ = nullptr;
        return;
    }

    IPluginFactory* factory = getFactory();
    if (!factory) {
        Trace::Error("VST3: GetPluginFactory returned null");
        VST3ModuleExitFunc moduleExit = (VST3ModuleExitFunc)dlsym(moduleHandle_, "ModuleExit");
        if (moduleExit) moduleExit();
        dlclose(moduleHandle_);
        moduleHandle_ = nullptr;
        return;
    }
    pluginFactory_ = factory;

    // Create the processor component
    IComponent* component = nullptr;
    tresult res = factory->createInstance(
        (FIDString)pluginClassId_,
        IComponent::iid,
        (void**)&component
    );
    if (res != kResultOk || !component) {
        Trace::Error("VST3: Failed to create component");
        cleanupPlugin();
        return;
    }
    component_ = component;

    // Initialize the component
    res = component->initialize(static_cast<FUnknown*>(&gHostApp));
    if (res != kResultOk) {
        Trace::Error("VST3: Component initialization failed");
        cleanupPlugin();
        return;
    }

    // Get IAudioProcessor
    IAudioProcessor* audioProc = nullptr;
    res = component->queryInterface(IAudioProcessor::iid, (void**)&audioProc);
    if (res != kResultOk || !audioProc) {
        Trace::Error("VST3: Component does not implement IAudioProcessor");
        cleanupPlugin();
        return;
    }
    audioProcessor_ = audioProc;

    // Try to get IEditController from the component directly
    IEditController* controller = nullptr;
    res = component->queryInterface(IEditController::iid, (void**)&controller);
    if (res == kResultOk && controller) {
        editController_ = controller;
        componentIsController_ = true;
        Trace::Log("VST3", "Component implements IEditController directly");
    } else {
        // Create separate controller
        TUID controllerCID;
        res = component->getControllerClassId(controllerCID);
        if (res == kResultOk) {
            Trace::Log("VST3", "Separate controller CID: %s",
                tuidToHex((const uint8_t*)controllerCID).c_str());
            res = factory->createInstance(
                controllerCID,
                IEditController::iid,
                (void**)&controller
            );
            if (res == kResultOk && controller) {
                controller->initialize(static_cast<FUnknown*>(&gHostApp));
                editController_ = controller;
                componentIsController_ = false;
                Trace::Log("VST3", "Separate controller created OK");
            } else {
                Trace::Error("VST3: createInstance for controller failed (res=%d)", (int)res);
            }
        } else {
            Trace::Error("VST3: getControllerClassId failed (res=%d)", (int)res);
        }
    }

    // Set component handler on controller
    if (editController_) {
        IEditController* ctrl = (IEditController*)editController_;
        SimpleComponentHandler* handler = new SimpleComponentHandler();
        ctrl->setComponentHandler(handler);
        handler->release(); // controller retains it

        // For separate controllers, connect them via IConnectionPoint
        // so the processor can send parameter definitions to the controller.
        if (!componentIsController_) {
            IConnectionPoint* compCP = nullptr;
            IConnectionPoint* ctrlCP = nullptr;
            component->queryInterface(IConnectionPoint::iid, (void**)&compCP);
            ctrl->queryInterface(IConnectionPoint::iid, (void**)&ctrlCP);
            if (compCP && ctrlCP) {
                compCP->connect(ctrlCP);
                ctrlCP->connect(compCP);
                Trace::Log("VST3", "Connected component <-> controller via IConnectionPoint");
            } else {
                Trace::Log("VST3", "IConnectionPoint not available (comp=%p ctrl=%p)",
                    (void*)compCP, (void*)ctrlCP);
            }
            if (compCP) compCP->release();
            if (ctrlCP) ctrlCP->release();
        }

        // Synchronize component state so the controller knows about
        // the component's default parameter values.
        if (!componentIsController_) {
            SimpleMemoryStream stream;
            tresult stRes = component->getState(&stream);
            if (stRes == kResultOk && stream.size() > 0) {
                stream.resetRead();
                ctrl->setComponentState(&stream);
                Trace::Log("VST3", "Synced component state to controller (%d bytes)",
                    (int)stream.size());
            } else {
                Trace::Log("VST3", "Component getState returned %d (size=%d)",
                    (int)stRes, (int)stream.size());
            }
        }
    }

    // Extract plugin name from factory class info
    int classCount = factory->countClasses();
    for (int i = 0; i < classCount; i++) {
        PClassInfo ci;
        if (factory->getClassInfo(i, &ci) == kResultOk) {
            if (memcmp(ci.cid, pluginClassId_, 16) == 0) {
                strncpy(name_, ci.name, sizeof(name_) - 1);
                name_[sizeof(name_) - 1] = '\0';
                break;
            }
        }
    }

    // Discover parameters
    discoverParameters();

    // Discover presets/programs via IUnitInfo
    discoverPresets();

    // Set up processing with a default buffer size
    int sampleRate = Audio::GetInstance()->GetSampleRate();
    if (sampleRate <= 0) sampleRate = 44100;
    // Don't set up processing yet — it will happen on first Render() call
    // when we know the actual buffer size
}

// ====================================================================
// discoverParameters: enumerate parameters from IEditController
// ====================================================================
void VST3Instrument::discoverParameters() {
    if (!editController_) {
        Trace::Error("VST3: No edit controller, cannot discover parameters");
        return;
    }

    IEditController* ctrl = (IEditController*)editController_;
    int32 paramCount = ctrl->getParameterCount();

    parameters_.clear();

    for (int32 i = 0; i < paramCount; i++) {
        ParameterInfo info;
        if (ctrl->getParameterInfo(i, info) != kResultOk) continue;

        // Skip hidden and read-only parameters
        if (info.flags & ParameterInfo::kIsHidden) continue;
        if (info.flags & ParameterInfo::kIsReadOnly) continue;

        VST3PluginParameter param;

        // Convert title from String128 (char16) to ASCII
        char titleBuf[128];
        string128ToChar(info.title, titleBuf, 128);
        param.name = titleBuf;

        char unitsBuf[128];
        string128ToChar(info.units, unitsBuf, 128);
        param.units = unitsBuf;

        param.paramId = info.id;
        param.stepCount = info.stepCount;
        param.defaultValue = ctrl->normalizedParamToPlain(info.id, info.defaultNormalizedValue);
        param.currentValue = info.defaultNormalizedValue;
        param.isReadOnly = (info.flags & ParameterInfo::kIsReadOnly) != 0;
        param.isBypass = (info.flags & ParameterInfo::kIsBypass) != 0;
        param.isList = (info.flags & ParameterInfo::kIsList) != 0;

        // Compute plain min/max
        param.minValue = ctrl->normalizedParamToPlain(info.id, 0.0);
        param.maxValue = ctrl->normalizedParamToPlain(info.id, 1.0);

        // Create a WatchedVariable for UI binding
        // Use 0-255 range for continuous, 0-stepCount for discrete
        int maxVal = (info.stepCount > 0 && info.stepCount <= 255) ? info.stepCount : 255;
        int defVal = (int)(info.defaultNormalizedValue * maxVal);

        // Use the VST3 paramId directly as the FourCC — it is unique per plugin
        FourCC varId = (FourCC)info.id;

        WatchedVariable *wv = new WatchedVariable(param.name.c_str(), varId, defVal);
        wv->AddObserver(*this);
        param.variable = wv;
        Insert(wv);

        parameters_.push_back(param);
    }

    Trace::Log("VST3", "Discovered %d parameters for %s", (int)parameters_.size(), name_);
}

// ====================================================================
// discoverPresets: enumerate program lists via IUnitInfo
// ====================================================================
void VST3Instrument::discoverPresets() {
    programLists_.clear();
    currentBank_ = 0;
    currentPreset_ = 0;
    programChangeParamIdx_ = -1;

    if (!editController_) return;

    IEditController* ctrl = (IEditController*)editController_;

    // Find the kIsProgramChange parameter (if any)
    int32 paramCount = ctrl->getParameterCount();
    for (int32 i = 0; i < paramCount; i++) {
        ParameterInfo info;
        if (ctrl->getParameterInfo(i, info) != kResultOk) continue;
        if (info.flags & ParameterInfo::kIsProgramChange) {
            // Find it in our discovered parameters_ vector
            for (int p = 0; p < (int)parameters_.size(); p++) {
                if (parameters_[p].paramId == info.id) {
                    programChangeParamIdx_ = p;
                    break;
                }
            }
            // If not found in parameters_ (e.g. it was hidden/readonly), use raw paramId
            if (programChangeParamIdx_ < 0) {
                // Create a synthetic entry so we can still use it
                Trace::Log("VST3", "Program change param %d not in parameters_ list, using raw", (int)info.id);
            }
            Trace::Log("VST3", "Found kIsProgramChange parameter: id=%d stepCount=%d",
                (int)info.id, (int)info.stepCount);
            break;
        }
    }

    // Try to get IUnitInfo from the edit controller
    IUnitInfo* unitInfo = nullptr;
    tresult res = ctrl->queryInterface(IUnitInfo::iid, (void**)&unitInfo);
    if (res != kResultOk || !unitInfo) {
        Trace::Log("VST3", "No IUnitInfo — plugin does not expose program lists");
        return;
    }

    int32 listCount = unitInfo->getProgramListCount();
    Trace::Log("VST3", "Found %d program list(s)", (int)listCount);

    for (int32 li = 0; li < listCount; li++) {
        ProgramListInfo plInfo;
        if (unitInfo->getProgramListInfo(li, plInfo) != kResultOk) continue;

        VST3ProgramList pl;
        pl.listId = plInfo.id;

        char nameBuf[128];
        string128ToChar(plInfo.name, nameBuf, 128);
        pl.name = nameBuf;

        for (int32 pi = 0; pi < plInfo.programCount; pi++) {
            String128 progName;
            if (unitInfo->getProgramName(plInfo.id, pi, progName) == kResultOk) {
                char progBuf[128];
                string128ToChar(progName, progBuf, 128);
                pl.programs.push_back(progBuf);
            } else {
                char fallback[32];
                snprintf(fallback, sizeof(fallback), "Program %d", (int)pi);
                pl.programs.push_back(fallback);
            }
        }

        Trace::Log("VST3", "  List[%d] '%s': %d programs", (int)plInfo.id,
            pl.name.c_str(), (int)pl.programs.size());

        programLists_.push_back(pl);
    }

    unitInfo->release();

    if (!programLists_.empty()) {
        Trace::Log("VST3", "Total: %d bank(s), first bank '%s' has %d presets",
            (int)programLists_.size(),
            programLists_[0].name.c_str(),
            (int)programLists_[0].programs.size());
    }
}

// ====================================================================
// Preset/program accessors
// ====================================================================
const char *VST3Instrument::GetBankName(int bankIdx) const {
    if (bankIdx >= 0 && bankIdx < (int)programLists_.size()) {
        return programLists_[bankIdx].name.c_str();
    }
    return "---";
}

void VST3Instrument::SetCurrentBank(int bankIdx) {
    if (bankIdx >= 0 && bankIdx < (int)programLists_.size()) {
        currentBank_ = bankIdx;
        currentPreset_ = 0;
    }
}

int VST3Instrument::GetPresetCount() const {
    if (currentBank_ >= 0 && currentBank_ < (int)programLists_.size()) {
        return (int)programLists_[currentBank_].programs.size();
    }
    return 0;
}

int VST3Instrument::GetPresetCountForBank(int bankIdx) const {
    if (bankIdx >= 0 && bankIdx < (int)programLists_.size()) {
        return (int)programLists_[bankIdx].programs.size();
    }
    return 0;
}

const char *VST3Instrument::GetPresetName(int presetIdx) const {
    if (currentBank_ >= 0 && currentBank_ < (int)programLists_.size()) {
        const VST3ProgramList &pl = programLists_[currentBank_];
        if (presetIdx >= 0 && presetIdx < (int)pl.programs.size()) {
            return pl.programs[presetIdx].c_str();
        }
    }
    return "---";
}

void VST3Instrument::SetPreset(int presetIdx) {
    if (currentBank_ < 0 || currentBank_ >= (int)programLists_.size()) return;
    const VST3ProgramList &pl = programLists_[currentBank_];
    if (presetIdx < 0 || presetIdx >= (int)pl.programs.size()) return;

    currentPreset_ = presetIdx;

    if (!editController_) return;
    IEditController* ctrl = (IEditController*)editController_;

    // VST3 selects programs by setting the kIsProgramChange parameter.
    // The normalized value maps program index over stepCount:
    //   normalized = programIndex / stepCount
    // We search for the parameter that has kIsProgramChange flag.
    int32 paramCount = ctrl->getParameterCount();
    for (int32 i = 0; i < paramCount; i++) {
        ParameterInfo info;
        if (ctrl->getParameterInfo(i, info) != kResultOk) continue;
        if (!(info.flags & ParameterInfo::kIsProgramChange)) continue;

        // Check if this parameter belongs to the current bank's unit
        // (For simplicity, accept any program-change param, or match unitId)
        double normalized = 0.0;
        if (info.stepCount > 0) {
            normalized = (double)presetIdx / (double)info.stepCount;
        }

        // Clamp
        if (normalized < 0.0) normalized = 0.0;
        if (normalized > 1.0) normalized = 1.0;

        // Notify the controller
        ctrl->setParamNormalized(info.id, normalized);

        // Queue the change for the audio processor
        PendingParamChange pc;
        pc.paramId = info.id;
        pc.value = normalized;
        pendingEventsMutex_.Lock();
        pendingParamChanges_.push_back(pc);
        pendingEventsMutex_.Unlock();

        // Also update the parameter variable if it exists in parameters_
        for (int p = 0; p < (int)parameters_.size(); p++) {
            if (parameters_[p].paramId == info.id && parameters_[p].variable) {
                int maxVal = (info.stepCount > 0 && info.stepCount <= 255) ? info.stepCount : 255;
                int intVal = (int)(normalized * maxVal);
                parameters_[p].variable->SetInt(intVal);
                parameters_[p].currentValue = normalized;
                break;
            }
        }

        Trace::Log("VST3", "Set preset %d ('%s') via param %d normalized=%.4f",
            presetIdx, GetPresetName(presetIdx), (int)info.id, normalized);
        break;
    }
}

// ====================================================================
// setupProcessing: configure the processor for audio
// ====================================================================
void VST3Instrument::setupProcessing(int bufferSize) {
    if (!audioProcessor_ || !component_) return;

    IAudioProcessor* proc = (IAudioProcessor*)audioProcessor_;
    IComponent* comp = (IComponent*)component_;

    int sampleRate = Audio::GetInstance()->GetSampleRate();
    if (sampleRate <= 0) sampleRate = 44100;

    // Check supported sample size
    if (proc->canProcessSampleSize(kSample32) != kResultOk) {
        Trace::Error("VST3: Plugin does not support 32-bit float processing");
        return;
    }

    // Set up bus arrangements — try stereo
    SpeakerArrangement stereo = 0x3; // kSpeakerL | kSpeakerR
    int32 numInputBuses = comp->getBusCount(kAudio, kInput);
    int32 numOutputBuses = comp->getBusCount(kAudio, kOutput);

    if (numOutputBuses > 0) {
        // Activate output bus 0
        comp->activateBus(kAudio, kOutput, 0, true);

        if (numInputBuses > 0) {
            comp->activateBus(kAudio, kInput, 0, true);
            SpeakerArrangement inputs[1] = { stereo };
            SpeakerArrangement outputs[1] = { stereo };
            proc->setBusArrangements(inputs, 1, outputs, 1);
        } else {
            SpeakerArrangement outputs[1] = { stereo };
            proc->setBusArrangements(nullptr, 0, outputs, 1);
        }
    }

    // Activate event (MIDI) buses
    int32 numEventInputBuses = comp->getBusCount(kEvent, kInput);
    for (int32 i = 0; i < numEventInputBuses; i++) {
        comp->activateBus(kEvent, kInput, i, true);
    }

    // Setup processing
    ProcessSetup setup;
    setup.processMode = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = bufferSize;
    setup.sampleRate = (double)sampleRate;

    tresult res = proc->setupProcessing(setup);
    if (res != kResultOk) {
        Trace::Error("VST3: setupProcessing failed");
        return;
    }

    // Activate
    res = comp->setActive(true);
    if (res != kResultOk) {
        Trace::Error("VST3: setActive(true) failed");
        return;
    }
    isActive_ = true;

    // Start processing
    res = proc->setProcessing(true);
    if (res != kResultOk) {
        Trace::Log("VST3", "setProcessing(true) returned non-OK (this is often fine)");
    }
    isProcessing_ = true;
}

// ====================================================================
// cleanupPlugin: release all VST3 resources
// ====================================================================
void VST3Instrument::cleanupPlugin() {
    if (audioProcessor_ && isProcessing_) {
        IAudioProcessor* proc = (IAudioProcessor*)audioProcessor_;
        proc->setProcessing(false);
        isProcessing_ = false;
    }

    if (component_ && isActive_) {
        IComponent* comp = (IComponent*)component_;
        comp->setActive(false);
        isActive_ = false;
    }

    // Release controller
    if (editController_ && !componentIsController_) {
        IEditController* ctrl = (IEditController*)editController_;
        ctrl->terminate();
        ctrl->release();
    }
    editController_ = nullptr;

    // Release audio processor
    if (audioProcessor_) {
        IAudioProcessor* proc = (IAudioProcessor*)audioProcessor_;
        proc->release();
        audioProcessor_ = nullptr;
    }

    // Release component
    if (component_) {
        IComponent* comp = (IComponent*)component_;
        comp->terminate();
        comp->release();
        component_ = nullptr;
    }

    // Unload module
    if (moduleHandle_) {
        VST3ModuleExitFunc moduleExit = (VST3ModuleExitFunc)dlsym(moduleHandle_, "ModuleExit");
        if (moduleExit) moduleExit();
        dlclose(moduleHandle_);
        moduleHandle_ = nullptr;
    }

    pluginFactory_ = nullptr;
    componentIsController_ = false;

    // Free audio buffers
    free(audioBufferL_); audioBufferL_ = nullptr;
    free(audioBufferR_); audioBufferR_ = nullptr;
    free(audioInputL_); audioInputL_ = nullptr;
    free(audioInputR_); audioInputR_ = nullptr;
    bufferSize_ = 0;

    // Clear preset data
    programLists_.clear();
    currentBank_ = 0;
    currentPreset_ = 0;
    programChangeParamIdx_ = -1;
}

// ====================================================================
// ScanPlugins: find all VST3 instrument plugins on the system
// ====================================================================
std::vector<VST3PluginInfo> VST3Instrument::ScanPlugins() {
    std::vector<VST3PluginInfo> result;

    // Standard Linux VST3 locations
    std::vector<std::string> searchPaths;
    const char* home = getenv("HOME");
    if (home) {
        searchPaths.push_back(std::string(home) + "/.vst3");
    }
    searchPaths.push_back("/usr/lib/vst3");
    searchPaths.push_back("/usr/local/lib/vst3");

    // Find all .vst3 bundles
    std::vector<std::string> bundles;
    for (auto& sp : searchPaths) {
        findVST3Bundles(sp, bundles);
    }

    // For each bundle, load temporarily to get class info
    for (auto& bundlePath : bundles) {
        std::string soPath = resolveVST3SOPath(bundlePath);
        if (soPath.empty()) continue;

        void* handle = dlopen(soPath.c_str(), RTLD_LAZY);
        if (!handle) continue;

        VST3ModuleEntryFunc entry = (VST3ModuleEntryFunc)dlsym(handle, "ModuleEntry");
        VST3ModuleExitFunc exit_fn = (VST3ModuleExitFunc)dlsym(handle, "ModuleExit");
        VST3GetFactoryFunc getFactory = (VST3GetFactoryFunc)dlsym(handle, "GetPluginFactory");

        if (!entry || !exit_fn || !getFactory) {
            dlclose(handle);
            continue;
        }

        if (!entry(handle)) {
            dlclose(handle);
            continue;
        }

        IPluginFactory* factory = getFactory();
        if (factory) {
            int count = factory->countClasses();

            // Try to get IPluginFactory2 for subcategory filtering
            IPluginFactory2* factory2 = nullptr;
            factory->queryInterface(IPluginFactory2::iid, (void**)&factory2);

            for (int i = 0; i < count; i++) {
                PClassInfo ci;
                if (factory->getClassInfo(i, &ci) == kResultOk) {
                    // Filter for Audio Module Class (VST3 instruments/effects)
                    if (strcmp(ci.category, kVstAudioEffectClass) == 0) {
                        // Check subcategories to filter for instruments only
                        bool isInstrument = false;
                        if (factory2) {
                            PClassInfo2 ci2;
                            if (factory2->getClassInfo2(i, &ci2) == kResultOk) {
                                // subCategories is a pipe-separated string like
                                // "Instrument|Synth" or "Fx|Delay"
                                if (strstr(ci2.subCategories, "Instrument") != nullptr) {
                                    isInstrument = true;
                                }
                            }
                        } else {
                            // No factory2 available — fall back to including
                            // all audio plugins (can't distinguish)
                            isInstrument = true;
                        }

                        if (!isInstrument) continue;

                        VST3PluginInfo info;
                        info.name = ci.name;
                        info.path = bundlePath;
                        memcpy(info.classId, ci.cid, 16);
                        info.classIdStr = tuidToHex((const uint8_t*)ci.cid);

                        // Skip Steam/Valve system plugins
                        if (info.name.find("Steam") != std::string::npos ||
                            info.name.find("Valve") != std::string::npos) {
                            continue;
                        }

                        result.push_back(info);
                    }
                }
            }

            if (factory2) factory2->release();
        }

        exit_fn();
        dlclose(handle);
    }

    // Sort by name
    std::sort(result.begin(), result.end(),
        [](const VST3PluginInfo& a, const VST3PluginInfo& b) {
            return a.name < b.name;
        });

    return result;
}

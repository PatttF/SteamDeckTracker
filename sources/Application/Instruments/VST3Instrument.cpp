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
#include "OsTIrusPatches.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <exception>
#include <fstream>
#include <future>
#include <memory>
#include <set>
#include <thread>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <zlib.h>
#include <zstd.h>

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
#include "pluginterfaces/vst/ivstprocesscontext.h"

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
        // Static instance — never actually delete
        if (refCount_ > 1) --refCount_;
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
    const uint8_t* data() const { return data_.empty() ? nullptr : &data_[0]; }
    void setData(const uint8_t* src, size_t len) {
        data_.assign(src, src + len);
        pos_ = 0;
    }

private:
    std::vector<uint8_t> data_;
    size_t pos_;
    int32_t refCount_;
};

// ====================================================================
// Base64 encode/decode helpers for storing binary state blobs in XML
// ====================================================================
static const char b64chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64Encode(const uint8_t *data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = ((uint32_t)data[i]) << 16;
        if (i + 1 < len) n |= ((uint32_t)data[i + 1]) << 8;
        if (i + 2 < len) n |= (uint32_t)data[i + 2];
        out += b64chars[(n >> 18) & 0x3F];
        out += b64chars[(n >> 12) & 0x3F];
        out += (i + 1 < len) ? b64chars[(n >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? b64chars[n & 0x3F] : '=';
    }
    return out;
}

static int b64val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static std::vector<uint8_t> base64Decode(const std::string &encoded) {
    std::vector<uint8_t> out;
    out.reserve((encoded.size() / 4) * 3);
    uint32_t buf = 0;
    int bits = 0;
    for (size_t i = 0; i < encoded.size(); i++) {
        int v = b64val(encoded[i]);
        if (v < 0) continue; // skip '=' and whitespace
        buf = (buf << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)(buf >> bits));
            buf &= (1u << bits) - 1;
        }
    }
    return out;
}

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
    projectTimeSamples_ = 0;

    currentBank_ = 0;
    currentPreset_ = 0;
    programChangeParamIdx_ = -1;
    usingFilePresets_ = false;
    usingMidiPresets_ = false;

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

                // Restore full component state blob first (this sets the
                // plugin's internal state, then syncs params to Variables)
                auto compIt = pendingParamValues_.find("vst3_comp_state");
                if (compIt != pendingParamValues_.end()) {
                    RestoreComponentState(compIt->second);
                    pendingParamValues_.erase(compIt);
                }

                // Restore controller state (if separate from component)
                auto ctrlIt = pendingParamValues_.find("vst3_ctrl_state");
                if (ctrlIt != pendingParamValues_.end()) {
                    RestoreControllerState(ctrlIt->second);
                    pendingParamValues_.erase(ctrlIt);
                }

                // Apply any remaining pending parameter values
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

    Trace::Log("VST3DBG", "Start ch=%d note=%d retrigger=%d", channel, (int)note, (int)retrigger);

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

    // Update tracking state under the same lock so Render() and Stop()
    // always see consistent lastNote_/playing_ vs queued events
    lastNote_[channel] = note;
    playing_[channel] = true;
    pendingEventsMutex_.Unlock();

    // Reset LFO phases on new note
    tremoloLFO_[channel].phase = 0;
    vibratoLFO_[channel].phase = 0;

    return true;
}

void VST3Instrument::Stop(int channel) {
    if (channel < 0 || channel >= SONG_CHANNEL_COUNT) return;

    pendingEventsMutex_.Lock();
    if (!playing_[channel]) {
        pendingEventsMutex_.Unlock();
        return;
    }

    // Queue note off
    PendingNoteEvent evt;
    evt.channel = 0;
    evt.pitch = lastNote_[channel];
    evt.velocity = 0.0f;
    evt.noteId = lastNote_[channel];
    pendingNoteEvents_.push_back(evt);

    playing_[channel] = false;
    pendingEventsMutex_.Unlock();
}

bool VST3Instrument::Render(int channel, fixed *buffer, int size, bool updateTick) {
    if (channel < 0 || channel >= SONG_CHANNEL_COUNT) {
        memset(buffer, 0, size * 2 * sizeof(fixed));
        return false;
    }

    // Snapshot playing state under lock (consistent with Start/Stop)
    bool noteActive;
    {
        pendingEventsMutex_.Lock();
        noteActive = playing_[channel];
        pendingEventsMutex_.Unlock();
    }

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

        // Use Lock() instead of TryLock() to guarantee we never silently
        // drop pending note events — losing a note-off causes stuck notes
        // and can crash the plugin on rapid retrigger.
        pendingEventsMutex_.Lock();
        renderLocalNotes_.swap(pendingNoteEvents_);
        renderLocalParams_.swap(pendingParamChanges_);
        renderLocalMidiCCs_.swap(pendingMidiCCs_);
        pendingEventsMutex_.Unlock();

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

        // Convert pending MIDI CC events to kLegacyMIDICCOutEvent
        for (auto& mc : renderLocalMidiCCs_) {
            Event e;
            memset(&e, 0, sizeof(e));
            e.busIndex = 0;
            e.sampleOffset = 0;
            e.type = Event::kLegacyMIDICCOutEvent;
            e.midiCCOut.controlNumber = mc.controlNumber;
            e.midiCCOut.channel = mc.channel;
            e.midiCCOut.value = mc.value;
            e.midiCCOut.value2 = mc.value2;
            eventList.addEvent(e);
        }
        renderLocalMidiCCs_.clear();

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

        // Fill output buffers with sentinel value to detect if yabridge writes them
        for (int i = 0; i < size; i++) {
            audioBufferL_[i] = 0.12345f;
            audioBufferR_[i] = 0.12345f;
        }
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

        // Provide a valid ProcessContext — many VST3 plugins (especially
        // Windows ones running via yabridge) produce silence without one.
        ProcessContext processContext;
        memset(&processContext, 0, sizeof(processContext));
        processContext.sampleRate = (double)Audio::GetInstance()->GetSampleRate();
        processContext.projectTimeSamples = projectTimeSamples_;
        processContext.tempo = 120.0; // default BPM
        processContext.timeSigNumerator = 4;
        processContext.timeSigDenominator = 4;
        processContext.state = ProcessContext::kPlaying
                             | ProcessContext::kTempoValid
                             | ProcessContext::kTimeSigValid
                             | ProcessContext::kContTimeValid;
        processContext.continousTimeSamples = projectTimeSamples_;
        processData.processContext = &processContext;

        projectTimeSamples_ += size;

        // Run the plugin
        processData.numSamples = size;
        int evtCount = eventList.getEventCount();
        float* preProcL = outputBus.channelBuffers32[0];
        float* preProcR = outputBus.channelBuffers32[1];
        tresult processResult = proc->process(processData);
        float* postProcL = outputBus.channelBuffers32[0];
        float* postProcR = outputBus.channelBuffers32[1];

        // If yabridge replaced the output buffer pointers, read from the new ones
        if (postProcL != preProcL || postProcR != preProcR) {
            Trace::Log("VST3DBG", "OUTPUT POINTERS CHANGED! pre=%p/%p post=%p/%p",
                (void*)preProcL, (void*)preProcR, (void*)postProcL, (void*)postProcR);
            // Copy from yabridge's buffers to ours
            if (postProcL) memcpy(audioBufferL_, postProcL, size * sizeof(float));
            if (postProcR) memcpy(audioBufferR_, postProcR, size * sizeof(float));
        }

        // Log every call that has events — read from the actual output pointers
        if (evtCount > 0) {
            float p = 0.0f;
            int sentinelCount = 0;
            float *checkBuf = postProcL ? postProcL : audioBufferL_;
            for (int i = 0; i < size && i < 32; i++) {
                float a = fabsf(checkBuf[i]);
                if (a > p) p = a;
                if (fabsf(checkBuf[i] - 0.12345f) < 0.0001f) sentinelCount++;
            }
            Trace::Log("VST3DBG", "EVENTS=%d  ret=%d  peak32=%.6f  sentinel=%d/32  outPtr=%p postPtr=%p", evtCount, (int)processResult, p, sentinelCount, (void*)audioBufferL_, (void*)postProcL);
        }

        // Debug: log process result and peak levels every ~1 second (86 calls at 44100/512)
        static int renderDbgCount = 0;
        if (++renderDbgCount >= 86) {
            renderDbgCount = 0;
            float peakL = 0.0f, peakR = 0.0f;
            for (int i = 0; i < size; i++) {
                float al = fabsf(audioBufferL_[i]);
                float ar = fabsf(audioBufferR_[i]);
                if (al > peakL) peakL = al;
                if (ar > peakR) peakR = ar;
            }
            // Also check output bus pointers and silence flags
            Trace::Log("VST3DBG", "process() ret=%d  size=%d  events=%d  peakL=%.6f  peakR=%.6f  active=%d processing=%d  outBufL=%p outBufR=%p silFlg=%llu",
                (int)processResult, size, eventList.getEventCount(),
                peakL, peakR, (int)isActive_, (int)isProcessing_,
                (void*)outputBus.channelBuffers32[0], (void*)outputBus.channelBuffers32[1],
                (unsigned long long)outputBus.silenceFlags);
        }

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
            fixed tl = fp_mul(buffer[i * 2], mul);
            fixed tr = fp_mul(buffer[i * 2 + 1], mul);
            if (tl > i2fp(32767)) tl = i2fp(32767);
            else if (tl < i2fp(-32768)) tl = i2fp(-32768);
            if (tr > i2fp(32767)) tr = i2fp(32767);
            else if (tr < i2fp(-32768)) tr = i2fp(-32768);
            buffer[i * 2]     = tl;
            buffer[i * 2 + 1] = tr;
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

            fixed rbL = fp_add(fp_mul(dryL, send), fbL);
            fixed rbR = fp_add(fp_mul(dryR, send), fbR);
            if (rbL > i2fp(32767)) rbL = i2fp(32767);
            else if (rbL < i2fp(-32768)) rbL = i2fp(-32768);
            if (rbR > i2fp(32767)) rbR = i2fp(32767);
            else if (rbR < i2fp(-32768)) rbR = i2fp(-32768);
            reverbBuf[reverbPos * 2] = rbL;
            reverbBuf[reverbPos * 2 + 1] = rbR;

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
// Full plugin state save/restore via IComponent/IEditController
// ====================================================================

std::string VST3Instrument::GetComponentStateBase64() const {
    if (!component_) return "";
    IComponent* comp = (IComponent*)component_;
    SimpleMemoryStream stream;
    tresult res = comp->getState(&stream);
    if (res != kResultOk || stream.size() == 0) return "";
    return base64Encode(stream.data(), stream.size());
}

std::string VST3Instrument::GetControllerStateBase64() const {
    if (!editController_) return "";
    IEditController* ctrl = (IEditController*)editController_;
    SimpleMemoryStream stream;
    tresult res = ctrl->getState(&stream);
    if (res != kResultOk || stream.size() == 0) return "";
    return base64Encode(stream.data(), stream.size());
}

bool VST3Instrument::RestoreComponentState(const std::string &base64Data) {
    if (!component_ || base64Data.empty()) return false;
    std::vector<uint8_t> blob = base64Decode(base64Data);
    if (blob.empty()) return false;

    IComponent* comp = (IComponent*)component_;
    SimpleMemoryStream stream;
    stream.setData(&blob[0], blob.size());

    tresult res = comp->setState(&stream);
    if (res != kResultOk) {
        Trace::Error("VST3: IComponent::setState failed (%d)", (int)res);
        return false;
    }

    // Also notify the controller about the component state
    if (editController_ && !componentIsController_) {
        IEditController* ctrl = (IEditController*)editController_;
        stream.resetRead();
        ctrl->setComponentState(&stream);
    }

    // Sync parameter values from the plugin back into our Variables
    if (editController_) {
        IEditController* ctrl = (IEditController*)editController_;
        for (size_t i = 0; i < parameters_.size(); ++i) {
            VST3PluginParameter &p = parameters_[i];
            double norm = ctrl->getParamNormalized(p.paramId);
            p.currentValue = norm;
            if (p.variable) {
                int maxVal = (p.stepCount > 0 && p.stepCount <= 255) ? p.stepCount : 255;
                int scaled = (int)(norm * maxVal + 0.5);
                if (scaled < 0) scaled = 0;
                if (scaled > maxVal) scaled = maxVal;
                p.variable->SetInt(scaled, false);
            }
        }
    }

    Trace::Log("VST3", "Restored component state (%d bytes)", (int)blob.size());
    return true;
}

bool VST3Instrument::RestoreControllerState(const std::string &base64Data) {
    if (!editController_ || base64Data.empty()) return false;
    std::vector<uint8_t> blob = base64Decode(base64Data);
    if (blob.empty()) return false;

    IEditController* ctrl = (IEditController*)editController_;
    SimpleMemoryStream stream;
    stream.setData(&blob[0], blob.size());

    tresult res = ctrl->setState(&stream);
    if (res != kResultOk) {
        Trace::Log("VST3", "IEditController::setState returned %d (may be unsupported)", (int)res);
        return false;
    }

    Trace::Log("VST3", "Restored controller state (%d bytes)", (int)blob.size());
    return true;
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

    // Wrap everything in a try/catch — yabridge's chainloader may throw
    // a C++ exception if the Wine host process fails to start or the
    // Windows plugin cannot be loaded (e.g. missing DLL dependencies).
    // Without this, std::terminate is called and the whole app aborts.
    try {
        loadPluginInner();
    } catch (const std::exception& ex) {
        Trace::Error("VST3: Exception loading plugin: %s", ex.what());
        cleanupPlugin();
    } catch (...) {
        Trace::Error("VST3: Unknown exception loading plugin %s", pluginPath_);
        cleanupPlugin();
    }
}

void VST3Instrument::loadPluginInner() {

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

    // If classId is all zeros (stub from generated moduleinfo.json for a
    // yabridge-bridged plugin), resolve the real CID from the factory.
    {
        bool cidIsZero = true;
        for (int i = 0; i < 16; i++) {
            if (pluginClassId_[i] != 0) { cidIsZero = false; break; }
        }
        if (cidIsZero) {
            int nClasses = factory->countClasses();
            IPluginFactory2* factory2 = nullptr;
            factory->queryInterface(IPluginFactory2::iid, (void**)&factory2);
            std::string resolvedSub;
            for (int i = 0; i < nClasses; i++) {
                PClassInfo ci;
                if (factory->getClassInfo(i, &ci) == kResultOk &&
                    strcmp(ci.category, kVstAudioEffectClass) == 0) {
                    memcpy(pluginClassId_, ci.cid, 16);
                    Trace::Log("VST3", "Resolved zero CID -> %s",
                               tuidToHex(pluginClassId_).c_str());
                    // Also grab subcategories so we can fix moduleinfo.json
                    if (factory2) {
                        PClassInfo2 ci2;
                        if (factory2->getClassInfo2(i, &ci2) == kResultOk) {
                            resolvedSub = ci2.subCategories;
                            Trace::Log("VST3", "Resolved subcategories: '%s'",
                                       resolvedSub.c_str());
                        }
                    }
                    // Update stored variable
                    Variable *plugVar = FindVariable(VST3IP_PLUGIN);
                    if (plugVar) {
                        std::string combined =
                            std::string(pluginPath_) + "|" +
                            tuidToHex(pluginClassId_);
                        plugVar->SetString(combined.c_str());
                    }
                    break;
                }
            }
            if (factory2) factory2->release();

            // Write the resolved CID and subcategories back to
            // moduleinfo.json so future scans can correctly distinguish
            // instruments from effects without loading the plugin.
            if (!resolvedSub.empty() || !cidIsZero) {
                std::string jsonPath =
                    std::string(pluginPath_) + "/Contents/moduleinfo.json";
                struct stat jsonSt;
                if (stat(jsonPath.c_str(), &jsonSt) == 0) {
                    // Extract plugin name from path
                    std::string stem = pluginPath_;
                    size_t sl = stem.rfind('/');
                    if (sl != std::string::npos) stem = stem.substr(sl + 1);
                    size_t ep = stem.rfind(".vst3");
                    if (ep != std::string::npos) stem = stem.substr(0, ep);

                    std::string json;
                    json += "{\n";
                    json += "  \"Classes\": [\n";
                    json += "    {\n";
                    json += "      \"CID\": \"" +
                            tuidToHex(pluginClassId_) + "\",\n";
                    json += "      \"Category\": \"Audio Module Class\",\n";
                    json += "      \"Name\": \"" + stem + "\",\n";
                    json += "      \"Sub Categories\": \"" +
                            resolvedSub + "\"\n";
                    json += "    }\n";
                    json += "  ]\n";
                    json += "}\n";

                    std::ofstream jf(jsonPath.c_str(),
                                     std::ios::out | std::ios::trunc);
                    if (jf.is_open()) {
                        jf << json;
                        jf.close();
                        Trace::Log("VST3",
                            "Updated moduleinfo.json: CID=%s sub='%s'",
                            tuidToHex(pluginClassId_).c_str(),
                            resolvedSub.c_str());
                    }
                }
            }
        }
    }

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

    // Set up processing now on the main thread so the potentially slow
    // IPC round-trips (yabridge/Wine: setActive, setProcessing) don't
    // happen on the audio render thread where they'd block and flood
    // the semaphore.
    {
        int sampleRate = Audio::GetInstance()->GetSampleRate();
        if (sampleRate <= 0) sampleRate = 44100;
        int initBlock = 4096; // generous maxSamplesPerBlock

        // Pre-allocate audio buffers
        audioBufferL_ = (float*)calloc(initBlock, sizeof(float));
        audioBufferR_ = (float*)calloc(initBlock, sizeof(float));
        audioInputL_ = (float*)calloc(initBlock, sizeof(float));
        audioInputR_ = (float*)calloc(initBlock, sizeof(float));
        bufferSize_ = initBlock;

        cachedOutputBuffer_ = (fixed*)calloc(initBlock * 2, sizeof(fixed));
        cachedOutputSize_ = initBlock;

        setupProcessing(initBlock);
        Trace::Log("VST3", "Pre-initialized processing (block=%d rate=%d)",
                   initBlock, sampleRate);

        // Force-load preset 0 via setParamNormalized on the controller —
        // some plugins (especially JUCE-based) don't load their init preset
        // automatically after initialize()+setActive().
        if (programChangeParamIdx_ >= 0 && editController_) {
            IEditController* ctrl = (IEditController*)editController_;
            ParameterInfo info;
            if (ctrl->getParameterInfo(0, info) == kResultOk || true) {
                // Use the stored program change param
                ParamID pcId = parameters_[programChangeParamIdx_].paramId;
                int stepCount = parameters_[programChangeParamIdx_].stepCount;
                double norm = (stepCount > 0) ? (1.0 / (double)stepCount) : 0.0; // preset 1
                ctrl->setParamNormalized(pcId, norm);

                // Queue it for the audio processor too
                PendingParamChange pc;
                pc.paramId = pcId;
                pc.value = norm;
                pendingEventsMutex_.Lock();
                pendingParamChanges_.push_back(pc);
                pendingEventsMutex_.Unlock();
                Trace::Log("VST3", "Force-loaded preset 0 via param %d", (int)pcId);
            }
        }
    }
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

// Helper: recursively find files with a given extension in a directory
static void findPresetFiles(const std::string &dir, const char *extension,
                            const std::string &category,
                            std::map<std::string, std::vector<VST3FilePreset>> &bankMap) {
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    struct dirent *ent;
    size_t extLen = strlen(extension);

    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        std::string name(ent->d_name);
        std::string full = dir + "/" + name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            // Recurse into subdirectory; use subdirectory name as category
            std::string subCat = category.empty() ? name : category + "/" + name;
            findPresetFiles(full, extension, subCat, bankMap);
        } else if (S_ISREG(st.st_mode)) {
            // Check extension
            if (name.size() > extLen &&
                name.substr(name.size() - extLen) == extension) {
                VST3FilePreset fp;
                fp.name = name.substr(0, name.size() - extLen);
                fp.filePath = full;
                std::string bankName = category.empty() ? "Presets" : category;
                bankMap[bankName].push_back(fp);
            }
        }
    }
    closedir(d);
}

// Table of known plugin-to-preset-directory mappings
static const VST3PluginPresetMapping knownPresetMappings[] = {
    // Surge XT: .fxp files with 60-byte FXP header to skip
    {
        "Surge XT",
        {
            "/usr/share/surge-xt/patches_factory",
            "/usr/share/surge-xt/patches_3rdparty",
            nullptr
        },
        ".fxp",
        60,  // CcnK(4) + byteSize(4) + FPCh(4) + version(4) + fxID(4) + fxVersion(4) +
             // numPrograms(4) + prgName(28) + chunkSize(4) = 60 bytes
        nullptr, 0
    },
    // Vital / Vitalium: .vital JSON files, no header to skip
    {
        "Vital",
        {
            nullptr  // No known preset directories installed; user can add their own
        },
        ".vital",
        0,
        nullptr, 0
    },
    // Serum 2: .SerumPreset files use the XferJson format, which is the same
    // format that IComponent::getState() produces. Feed entire file to setState.
    // (.fxp files contain Serum 1 binary chunk data which is incompatible.)
    {
        "Serum",
        {
            nullptr  // Directories resolved at runtime from WINEPREFIX
        },
        ".SerumPreset",
        0,   // No header skip — XferJson header is part of the state format
        nullptr, 0
    },
    // Sentinel
    { nullptr, {nullptr}, nullptr, 0, nullptr, 0 }
};

// ====================================================================
// discoverPresets: enumerate program lists via IUnitInfo
// ====================================================================

void VST3Instrument::discoverPresets() {
    programLists_.clear();
    filePresetsByBank_.clear();
    usingFilePresets_ = false;
    usingMidiPresets_ = false;
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
        Trace::Log("VST3", "No IUnitInfo — trying file-based preset scanning");
        discoverPresetFiles();
        if (!usingFilePresets_) {
            discoverHardcodedPresets();
        }
        if (!usingFilePresets_ && !usingMidiPresets_) {
            discoverPatchManagerPresets();
        }
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

    // If this plugin has a known file-preset mapping, always prefer file-based
    // presets over IUnitInfo programs (which are often just empty "Init" slots
    // for JUCE-based plugins like Serum 2, Vital, etc.).
    bool hasKnownMapping = false;
    for (int i = 0; knownPresetMappings[i].pluginNameSubstring != nullptr; i++) {
        if (strstr(name_, knownPresetMappings[i].pluginNameSubstring) != nullptr) {
            hasKnownMapping = true;
            break;
        }
    }

    if (hasKnownMapping) {
        Trace::Log("VST3", "Known preset mapping found for '%s' — "
            "trying file-based presets (overriding IUnitInfo)", name_);
        discoverPresetFiles();
    }

    // If file scanning didn't find presets, check if IUnitInfo had useful ones
    bool hasUsefulPresets = false;
    if (!usingFilePresets_) {
        for (size_t i = 0; i < programLists_.size(); i++) {
            if (programLists_[i].programs.size() > 1) {
                hasUsefulPresets = true;
                break;
            }
        }
        if (!hasUsefulPresets) {
            // No file presets and no useful IUnitInfo presets — try other methods
            discoverPresetFiles();
        }
    }

    // If file-based scanning also didn't find anything, try hardcoded ROM presets
    if (!usingFilePresets_ && !hasUsefulPresets) {
        discoverHardcodedPresets();
    }

    // If no hardcoded presets either, try patchmanager cache (other gearmulator)
    if (!usingFilePresets_ && !usingMidiPresets_ && !hasUsefulPresets) {
        discoverPatchManagerPresets();
    }
}

// ====================================================================
// discoverPresetFiles: scan filesystem for native preset files
// ====================================================================

void VST3Instrument::discoverPresetFiles() {
    // Find a matching plugin mapping
    const VST3PluginPresetMapping *mapping = nullptr;
    for (int i = 0; knownPresetMappings[i].pluginNameSubstring != nullptr; i++) {
        if (strstr(name_, knownPresetMappings[i].pluginNameSubstring) != nullptr) {
            mapping = &knownPresetMappings[i];
            break;
        }
    }

    if (!mapping) {
        Trace::Log("VST3", "No known preset file mapping for '%s'", name_);
        return;
    }

    Trace::Log("VST3", "Scanning preset files for '%s' (ext: %s, skip: %d)",
        name_, mapping->extension, mapping->headerSkipBytes);

    // Also check user home directories
    std::string homeDir;
    const char *home = getenv("HOME");
    if (home) homeDir = home;

    // Collect directories to scan
    std::vector<std::string> scanDirs;
    for (int i = 0; i < 6 && mapping->directories[i] != nullptr; i++) {
        scanDirs.push_back(mapping->directories[i]);
    }

    // Add user-specific directories based on plugin name
    if (!homeDir.empty()) {
        if (strstr(name_, "Surge XT") != nullptr) {
            scanDirs.push_back(homeDir + "/Documents/Surge XT/Patches");
        } else if (strstr(name_, "Vital") != nullptr) {
            scanDirs.push_back(homeDir + "/Documents/Vital");
            scanDirs.push_back(homeDir + "/.vital/User/Presets");
            scanDirs.push_back(homeDir + "/Music/Vital");
        } else if (strstr(name_, "Serum") != nullptr) {
            // Serum 2 presets live inside the Wine prefix.
            // Try WINEPREFIX env first, then the default SDTracker prefix.
            std::string pfxBase;
            const char *wpEnv = getenv("WINEPREFIX");
            if (wpEnv && wpEnv[0]) {
                pfxBase = wpEnv;
            } else {
                pfxBase = homeDir + "/Documents/SDTracker/wineprefix";
            }
            // Try both with and without /pfx (Proton may nest it)
            const char *userNames[] = { "steamuser", nullptr };
            const char *prefixes[] = { "", "/pfx", nullptr };
            for (int pi = 0; prefixes[pi] != nullptr; pi++) {
                for (int ui = 0; userNames[ui] != nullptr; ui++) {
                    std::string base = pfxBase + prefixes[pi]
                        + "/drive_c/users/" + userNames[ui]
                        + "/Documents/Xfer";
                    scanDirs.push_back(base + "/Serum 2 Presets/Presets");
                    scanDirs.push_back(base + "/Serum Presets/Presets");
                }
            }
        }
    }

    // Scan all directories and collect presets by category
    std::map<std::string, std::vector<VST3FilePreset>> bankMap;
    for (size_t i = 0; i < scanDirs.size(); i++) {
        findPresetFiles(scanDirs[i], mapping->extension, "", bankMap);
    }

    // Also scan for the second extension if present
    if (mapping->extension2) {
        for (size_t i = 0; i < scanDirs.size(); i++) {
            findPresetFiles(scanDirs[i], mapping->extension2, "", bankMap);
        }
    }

    if (bankMap.empty()) {
        Trace::Log("VST3", "No preset files found for '%s'", name_);
        return;
    }

    // Clear any existing IUnitInfo-based presets and switch to file mode
    programLists_.clear();
    filePresetsByBank_.clear();
    usingFilePresets_ = true;

    // Convert bankMap into programLists_ and filePresetsByBank_
    for (std::map<std::string, std::vector<VST3FilePreset>>::iterator it = bankMap.begin();
         it != bankMap.end(); ++it) {
        VST3ProgramList pl;
        pl.listId = (int32_t)programLists_.size();
        pl.name = it->first;

        std::vector<VST3FilePreset> bankFilePresets;
        for (size_t j = 0; j < it->second.size(); j++) {
            pl.programs.push_back(it->second[j].name);
            bankFilePresets.push_back(it->second[j]);
        }

        programLists_.push_back(pl);
        filePresetsByBank_.push_back(bankFilePresets);
    }

    currentBank_ = 0;
    currentPreset_ = 0;

    Trace::Log("VST3", "File presets: %d bank(s), total presets across banks",
        (int)programLists_.size());
    for (size_t i = 0; i < programLists_.size(); i++) {
        Trace::Log("VST3", "  Bank '%s': %d presets",
            programLists_[i].name.c_str(), (int)programLists_[i].programs.size());
    }
}

// ====================================================================
// discoverHardcodedPresets: use built-in ROM patch names for OsTIrus
// ====================================================================
void VST3Instrument::discoverHardcodedPresets() {
    if (strstr(name_, "OsTIrus") == nullptr) return;

    Trace::Log("VST3", "Using hardcoded ROM presets for OsTIrus");

    programLists_.clear();
    for (int b = 0; b < OSTIRUS_BANK_COUNT; b++) {
        VST3ProgramList pl;
        pl.listId = b;
        pl.name = ostirusBankNames[b];
        for (int p = 0; p < OSTIRUS_PATCHES_PER_BANK; p++) {
            pl.programs.push_back(ostirusPatchNames[b][p]);
        }
        programLists_.push_back(pl);
    }

    usingMidiPresets_ = true;
    currentBank_ = 0;
    currentPreset_ = 0;

    Trace::Log("VST3", "Hardcoded presets: %d banks x %d patches",
        OSTIRUS_BANK_COUNT, OSTIRUS_PATCHES_PER_BANK);
}

// ====================================================================
// discoverPatchManagerPresets: parse gearmulator patchmanager cache
// for OsTIrus/Osirus/etc. plugins that embed presets in ROM
// ====================================================================
//
// Binary format of patchmanagerdb.cache:
//   Pmpm(4) + version(u32) + totalSize(u32)             -- file header
//   PmDs(4) + version(u32) + size(u32) + bankCount(u32) -- dataset container
//   For each bank:
//     DatS(4) + version(u32) + payloadSize(u32)          -- bank header
//     flag(u16) + nameLen(u16) + pad(u16)                 -- bank meta
//     name[nameLen]                                       -- bank name (ASCII)
//     pad(u32) + patchCount(u32)                          -- patch count
//     For each patch:
//       Patc(4) + version(u32) + payloadSize(u32)         -- patch header
//       nameLen(u32) + name[nameLen]                       -- patch name (ASCII)
//       ... (tags, sysex data — skipped via payloadSize)

// Table of gearmulator plugin names and their patchmanager cache locations
struct PatchManagerMapping {
    const char *pluginNameSubstring;  // substring to match in plugin name
    const char *cacheDirSuffix;       // relative to ~/.local/share/The Usual Suspects/
};

static const PatchManagerMapping patchManagerMappings[] = {
    { "OsTIrus",  "OsTIrus" },
    { "Osirus",   "Osirus" },
    { "Vavra",    "Vavra" },
    { "Xenia",    "Xenia" },
    { nullptr, nullptr }
};

void VST3Instrument::discoverPatchManagerPresets() {
    // Check if this plugin matches a known gearmulator plugin
    const PatchManagerMapping *mapping = nullptr;
    for (int i = 0; patchManagerMappings[i].pluginNameSubstring != nullptr; i++) {
        if (strstr(name_, patchManagerMappings[i].pluginNameSubstring) != nullptr) {
            mapping = &patchManagerMappings[i];
            break;
        }
    }
    if (!mapping) return;

    // Build the cache file path
    const char *home = getenv("HOME");
    if (!home) return;

    std::string cachePath = std::string(home) +
        "/.local/share/The Usual Suspects/" +
        mapping->cacheDirSuffix +
        "/patchmanager/patchmanagerdb.cache";

    Trace::Log("VST3", "Looking for patchmanager cache: %s", cachePath.c_str());

    // Open with POSIX I/O (avoid project fopen macro)
    int fd = open(cachePath.c_str(), O_RDONLY);
    if (fd < 0) {
        Trace::Log("VST3", "No patchmanager cache found for '%s'", name_);
        return;
    }

    off_t fileSize = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);

    if (fileSize < 28) {  // minimum: Pmpm(12) + PmDs(16)
        Trace::Error("VST3: Patchmanager cache too small (%ld bytes)", (long)fileSize);
        close(fd);
        return;
    }

    // Read entire file into memory
    std::vector<uint8_t> data(fileSize);
    ssize_t bytesRead = read(fd, &data[0], fileSize);
    close(fd);

    if (bytesRead != fileSize) {
        Trace::Error("VST3: Short read on patchmanager cache");
        return;
    }

    // Helper lambdas for reading fields
    const uint8_t *buf = &data[0];
    size_t total = (size_t)fileSize;
    size_t pos = 0;

    auto readU32 = [&](uint32_t &val) -> bool {
        if (pos + 4 > total) return false;
        val = (uint32_t)buf[pos] | ((uint32_t)buf[pos+1] << 8) |
              ((uint32_t)buf[pos+2] << 16) | ((uint32_t)buf[pos+3] << 24);
        pos += 4;
        return true;
    };
    auto readU16 = [&](uint16_t &val) -> bool {
        if (pos + 2 > total) return false;
        val = (uint16_t)buf[pos] | ((uint16_t)buf[pos+1] << 8);
        pos += 2;
        return true;
    };
    auto readTag = [&](char tag[5]) -> bool {
        if (pos + 4 > total) return false;
        tag[0] = buf[pos]; tag[1] = buf[pos+1];
        tag[2] = buf[pos+2]; tag[3] = buf[pos+3]; tag[4] = '\0';
        pos += 4;
        return true;
    };

    // Parse file header: Pmpm + version + totalSize
    char tag[5];
    uint32_t ver, sz;
    if (!readTag(tag) || strcmp(tag, "Pmpm") != 0) {
        Trace::Error("VST3: Invalid patchmanager cache header");
        return;
    }
    if (!readU32(ver) || !readU32(sz)) return;

    // Parse PmDs container: PmDs + version + size + bankCount
    if (!readTag(tag) || strcmp(tag, "PmDs") != 0) {
        Trace::Error("VST3: Expected PmDs container in patchmanager cache");
        return;
    }
    uint32_t pmdsVer, pmdsSize, bankCount;
    if (!readU32(pmdsVer) || !readU32(pmdsSize) || !readU32(bankCount)) return;

    Trace::Log("VST3", "Patchmanager cache: %d banks", (int)bankCount);

    // Sanity limit
    if (bankCount > 256) {
        Trace::Error("VST3: Unreasonable bank count %d in patchmanager cache", (int)bankCount);
        return;
    }

    // Clear any existing preset data
    programLists_.clear();
    filePresetsByBank_.clear();

    for (uint32_t b = 0; b < bankCount; b++) {
        // Parse DatS header
        if (!readTag(tag) || strcmp(tag, "DatS") != 0) {
            Trace::Error("VST3: Expected DatS at bank %d, got '%s'", (int)b, tag);
            return;
        }
        uint32_t dsVer, dsSize;
        if (!readU32(dsVer) || !readU32(dsSize)) return;
        size_t payloadStart = pos;

        // DatS payload: flag(u16) + nameLen(u16) + pad(u16) + name + pad(u32) + patchCount(u32)
        uint16_t flag, nameLen, pad16;
        if (!readU16(flag) || !readU16(nameLen) || !readU16(pad16)) {
            pos = payloadStart + dsSize;
            continue;
        }

        // Read bank name
        char bankNameBuf[64];
        if (nameLen > 63) nameLen = 63;
        if (pos + nameLen > total) {
            pos = payloadStart + dsSize;
            continue;
        }
        memcpy(bankNameBuf, buf + pos, nameLen);
        bankNameBuf[nameLen] = '\0';
        // Trim trailing whitespace/nulls
        for (int i = (int)nameLen - 1; i >= 0 && (bankNameBuf[i] == '\0' || bankNameBuf[i] == ' '); i--)
            bankNameBuf[i] = '\0';
        pos += nameLen;

        uint32_t pad32, patchCount;
        if (!readU32(pad32) || !readU32(patchCount)) {
            pos = payloadStart + dsSize;
            continue;
        }

        VST3ProgramList pl;
        pl.listId = (int32_t)b;
        pl.name = bankNameBuf;

        // Parse patches
        for (uint32_t p = 0; p < patchCount && pos < total; p++) {
            if (!readTag(tag) || strcmp(tag, "Patc") != 0) {
                Trace::Error("VST3: Expected Patc at bank %d patch %d, got '%s'", (int)b, (int)p, tag);
                pos = payloadStart + dsSize;
                break;
            }
            uint32_t pVer, pSize;
            if (!readU32(pVer) || !readU32(pSize)) {
                pos = payloadStart + dsSize;
                break;
            }
            size_t pPayload = pos;

            // Patch payload: nameLen(u32) + name
            uint32_t pNameLen;
            if (!readU32(pNameLen)) {
                pos = pPayload + pSize;
                continue;
            }
            char patchNameBuf[32];
            uint32_t copyLen = pNameLen;
            if (copyLen > 31) copyLen = 31;
            if (pos + copyLen > total) {
                pos = pPayload + pSize;
                continue;
            }
            memcpy(patchNameBuf, buf + pos, copyLen);
            patchNameBuf[copyLen] = '\0';
            // Trim trailing whitespace/nulls
            for (int i = (int)copyLen - 1; i >= 0 && (patchNameBuf[i] == '\0' || patchNameBuf[i] == ' '); i--)
                patchNameBuf[i] = '\0';

            pl.programs.push_back(patchNameBuf);

            // Skip to end of patch payload
            pos = pPayload + pSize;
        }

        if (pl.programs.size() > 0) {
            programLists_.push_back(pl);
        }

        // Ensure we're at the right position for the next bank
        pos = payloadStart + dsSize;
    }

    if (programLists_.empty()) {
        Trace::Log("VST3", "No patches found in patchmanager cache");
        return;
    }

    // Presets will be selected by sending MIDI bank select + program change
    // via kLegacyMIDICCOutEvent in the VST3 event list. JUCE (used by gearmulator)
    // converts these to real MIDI messages that the Virus emulator processes.
    usingMidiPresets_ = true;

    currentBank_ = 0;
    currentPreset_ = 0;

    Trace::Log("VST3", "Patchmanager presets: %d bank(s), will use MIDI for selection",
        (int)programLists_.size());
    for (size_t i = 0; i < programLists_.size() && i < 5; i++) {
        Trace::Log("VST3", "  Bank '%s': %d presets",
            programLists_[i].name.c_str(), (int)programLists_[i].programs.size());
    }
    if (programLists_.size() > 5) {
        Trace::Log("VST3", "  ... and %d more banks", (int)(programLists_.size() - 5));
    }
}

// ====================================================================
// ====================================================================
// Minimal CBOR utilities for merging Serum 2 preset state
// ====================================================================
// Serum 2 uses XferJson format: "XferJson\0" (9 bytes) + 8-byte LE JSON length
// + JSON string + 4-byte LE decompressed size + 4-byte LE version (always 2)
// + zstd-compressed CBOR data.
//
// The CBOR is a map of module names (strings) to module data (maps).
// Each module map has "plainParams" (map of param names to float/string values)
// and optional non-param keys (curveData, clip, WTOsc, FX, etc.).
//
// getState() returns ALL modules with ALL params (full state).
// .SerumPreset files contain only CHANGED params plus metadata keys.
// We merge preset values into the getState CBOR, then re-encode and setState.
//
// Tree-based CBOR decode/encode/merge with MD5 hash for Serum 2 XferJson presets

namespace CborMerge {

// ====================================================================
// MD5 hash computation (RFC 1321)
// ====================================================================
static inline uint32_t md5F(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | (~x & z); }
static inline uint32_t md5G(uint32_t x, uint32_t y, uint32_t z) { return (x & z) | (y & ~z); }
static inline uint32_t md5H(uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; }
static inline uint32_t md5I(uint32_t x, uint32_t y, uint32_t z) { return y ^ (x | ~z); }
static inline uint32_t md5Rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void md5Transform(uint32_t state[4], const uint8_t block[64]) {
    static const uint32_t T[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,
        0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
        0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,
        0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,
        0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
        0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,
        0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,
        0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
        0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
    };
    static const int S[64] = {
        7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
        5, 9,14,20,5, 9,14,20,5, 9,14,20,5, 9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
        6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
    };
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t M[16];
    for (int i = 0; i < 16; i++) {
        M[i] = ((uint32_t)block[i*4]) | ((uint32_t)block[i*4+1] << 8) |
               ((uint32_t)block[i*4+2] << 16) | ((uint32_t)block[i*4+3] << 24);
    }
    for (int i = 0; i < 64; i++) {
        uint32_t f, g;
        if (i < 16)      { f = md5F(b,c,d); g = (uint32_t)i; }
        else if (i < 32)  { f = md5G(b,c,d); g = (uint32_t)((5*i+1) % 16); }
        else if (i < 48)  { f = md5H(b,c,d); g = (uint32_t)((3*i+5) % 16); }
        else               { f = md5I(b,c,d); g = (uint32_t)((7*i) % 16); }
        uint32_t tmp = d;
        d = c; c = b;
        b = b + md5Rotl(a + f + T[i] + M[g], S[i]);
        a = tmp;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
}

static std::string md5Hex(const uint8_t *data, size_t len) {
    uint32_t state[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    size_t i = 0;
    for (; i + 64 <= len; i += 64)
        md5Transform(state, data + i);
    uint8_t buf[128];
    memset(buf, 0, sizeof(buf));
    size_t rem = len - i;
    memcpy(buf, data + i, rem);
    buf[rem] = 0x80;
    size_t padLen = (rem < 56) ? 64 : 128;
    uint64_t bitLen = (uint64_t)len * 8;
    for (int k = 0; k < 8; k++)
        buf[padLen - 8 + k] = (uint8_t)(bitLen >> (8 * k));
    for (size_t j = 0; j < padLen; j += 64)
        md5Transform(state, buf + j);
    uint8_t digest[16];
    for (int k = 0; k < 4; k++) {
        digest[k*4+0] = (uint8_t)(state[k]);
        digest[k*4+1] = (uint8_t)(state[k] >> 8);
        digest[k*4+2] = (uint8_t)(state[k] >> 16);
        digest[k*4+3] = (uint8_t)(state[k] >> 24);
    }
    char hex[33];
    for (int k = 0; k < 16; k++)
        sprintf(hex + k*2, "%02x", digest[k]);
    hex[32] = '\0';
    return std::string(hex);
}

// ====================================================================
// JSON hash update — replace the "hash":"..." value in-place
// ====================================================================
static std::string updateJsonHash(const std::string &json, const std::string &newHash) {
    std::string marker = "\"hash\":\"";
    size_t pos = json.find(marker);
    if (pos == std::string::npos) return json;
    size_t hashStart = pos + marker.size();
    size_t hashEnd = json.find('"', hashStart);
    if (hashEnd == std::string::npos) return json;
    return json.substr(0, hashStart) + newHash + json.substr(hashEnd);
}

// ====================================================================
// CborValue — recursive tree type for CBOR data
// ====================================================================
struct CborValue {
    enum Type {
        T_UINT, T_NEGINT, T_FLOAT, T_DOUBLE, T_BOOL, T_NULL, T_UNDEFINED,
        T_STRING, T_BYTES, T_ARRAY, T_MAP, T_TAG
    };
    Type type;
    uint64_t uint_val;    // T_UINT value, T_NEGINT raw value (actual = -1-val), T_TAG number
    double float_val;     // T_FLOAT, T_DOUBLE
    bool bool_val;        // T_BOOL
    std::string str_val;  // T_STRING
    std::vector<uint8_t> bytes_val; // T_BYTES
    std::vector<CborValue> arr_val; // T_ARRAY items, T_TAG content (1 element)
    std::vector<std::pair<CborValue, CborValue> > map_val; // T_MAP ordered pairs

    CborValue() : type(T_NULL), uint_val(0), float_val(0), bool_val(false) {}

    bool isString() const { return type == T_STRING; }
    bool isMap() const { return type == T_MAP; }

    // Map lookup by string key — returns pointer or NULL
    CborValue *mapFind(const std::string &key) {
        for (size_t i = 0; i < map_val.size(); i++) {
            if (map_val[i].first.type == T_STRING && map_val[i].first.str_val == key)
                return &map_val[i].second;
        }
        return NULL;
    }
    const CborValue *mapFind(const std::string &key) const {
        for (size_t i = 0; i < map_val.size(); i++) {
            if (map_val[i].first.type == T_STRING && map_val[i].first.str_val == key)
                return &map_val[i].second;
        }
        return NULL;
    }
    // Set a map entry (update existing or append)
    void mapSet(const std::string &key, const CborValue &val) {
        for (size_t i = 0; i < map_val.size(); i++) {
            if (map_val[i].first.type == T_STRING && map_val[i].first.str_val == key) {
                map_val[i].second = val;
                return;
            }
        }
        CborValue k;
        k.type = T_STRING;
        k.str_val = key;
        map_val.push_back(std::make_pair(k, val));
    }
};

// ====================================================================
// CBOR decoder — recursive descent
// ====================================================================
static CborValue decodeCbor(const uint8_t *data, size_t len, size_t &pos) {
    CborValue val;
    if (pos >= len) return val;

    uint8_t ib = data[pos++];
    int mt = (ib >> 5) & 7;
    uint8_t ai = ib & 0x1F;

    // Decode additional info to get the argument value
    uint64_t arg = 0;
    bool isBreak = false;
    if (ai < 24) {
        arg = ai;
    } else if (ai == 24 && pos < len) {
        arg = data[pos++];
    } else if (ai == 25 && pos + 2 <= len) {
        arg = ((uint64_t)data[pos] << 8) | data[pos+1];
        pos += 2;
    } else if (ai == 26 && pos + 4 <= len) {
        arg = ((uint64_t)data[pos] << 24) | ((uint64_t)data[pos+1] << 16) |
              ((uint64_t)data[pos+2] << 8) | data[pos+3];
        pos += 4;
    } else if (ai == 27 && pos + 8 <= len) {
        for (int i = 0; i < 8; i++) arg = (arg << 8) | data[pos++];
    } else if (ai == 31) {
        isBreak = true;
        arg = UINT64_MAX;
    }

    switch (mt) {
    case 0: // unsigned integer
        val.type = CborValue::T_UINT;
        val.uint_val = arg;
        break;
    case 1: // negative integer (-1 - arg)
        val.type = CborValue::T_NEGINT;
        val.uint_val = arg;
        break;
    case 2: // byte string
        val.type = CborValue::T_BYTES;
        if (isBreak) {
            while (pos < len && data[pos] != 0xFF) {
                CborValue chunk = decodeCbor(data, len, pos);
                if (chunk.type == CborValue::T_BYTES)
                    val.bytes_val.insert(val.bytes_val.end(),
                        chunk.bytes_val.begin(), chunk.bytes_val.end());
            }
            if (pos < len) pos++; // skip break
        } else {
            size_t n = (size_t)arg;
            if (pos + n <= len) {
                val.bytes_val.assign(data + pos, data + pos + n);
                pos += n;
            }
        }
        break;
    case 3: // text string
        val.type = CborValue::T_STRING;
        if (isBreak) {
            while (pos < len && data[pos] != 0xFF) {
                CborValue chunk = decodeCbor(data, len, pos);
                if (chunk.type == CborValue::T_STRING)
                    val.str_val += chunk.str_val;
            }
            if (pos < len) pos++;
        } else {
            size_t n = (size_t)arg;
            if (pos + n <= len) {
                val.str_val.assign((const char*)data + pos, n);
                pos += n;
            }
        }
        break;
    case 4: // array
        val.type = CborValue::T_ARRAY;
        if (isBreak) {
            while (pos < len && data[pos] != 0xFF)
                val.arr_val.push_back(decodeCbor(data, len, pos));
            if (pos < len) pos++;
        } else {
            val.arr_val.reserve((size_t)arg);
            for (uint64_t i = 0; i < arg && pos < len; i++)
                val.arr_val.push_back(decodeCbor(data, len, pos));
        }
        break;
    case 5: // map
        val.type = CborValue::T_MAP;
        if (isBreak) {
            while (pos < len && data[pos] != 0xFF) {
                CborValue k = decodeCbor(data, len, pos);
                CborValue v = decodeCbor(data, len, pos);
                val.map_val.push_back(std::make_pair(k, v));
            }
            if (pos < len) pos++;
        } else {
            val.map_val.reserve((size_t)arg);
            for (uint64_t i = 0; i < arg && pos < len; i++) {
                CborValue k = decodeCbor(data, len, pos);
                CborValue v = decodeCbor(data, len, pos);
                val.map_val.push_back(std::make_pair(k, v));
            }
        }
        break;
    case 6: // tag
        val.type = CborValue::T_TAG;
        val.uint_val = arg;
        val.arr_val.push_back(decodeCbor(data, len, pos));
        break;
    case 7: // simple/float
        if (ai == 20)      { val.type = CborValue::T_BOOL; val.bool_val = false; }
        else if (ai == 21)  { val.type = CborValue::T_BOOL; val.bool_val = true; }
        else if (ai == 22)  { val.type = CborValue::T_NULL; }
        else if (ai == 23)  { val.type = CborValue::T_UNDEFINED; }
        else if (ai == 25) {
            // Half-precision float (16-bit IEEE 754)
            uint16_t h = (uint16_t)arg;
            int sign = (h >> 15) & 1;
            int exp = (h >> 10) & 0x1F;
            int mant = h & 0x3FF;
            double fval;
            if (exp == 0)       fval = ldexp(mant, -24);
            else if (exp == 31) fval = (mant == 0) ? INFINITY : NAN;
            else                fval = ldexp(mant + 1024, exp - 25);
            if (sign) fval = -fval;
            val.type = CborValue::T_FLOAT;
            val.float_val = fval;
        } else if (ai == 26) {
            // Single-precision float (IEEE 754)
            uint32_t bits = (uint32_t)arg;
            float f;
            memcpy(&f, &bits, sizeof(f));
            val.type = CborValue::T_FLOAT;
            val.float_val = (double)f;
        } else if (ai == 27) {
            // Double-precision float (IEEE 754)
            double d;
            memcpy(&d, &arg, sizeof(d));
            val.type = CborValue::T_DOUBLE;
            val.float_val = d;
        }
        break;
    }
    return val;
}

// ====================================================================
// CBOR encoder
// ====================================================================
static void writeHead(std::vector<uint8_t> &out, int mt, uint64_t val) {
    uint8_t major = (uint8_t)(mt << 5);
    if (val < 24) {
        out.push_back(major | (uint8_t)val);
    } else if (val <= 0xFF) {
        out.push_back(major | 24);
        out.push_back((uint8_t)val);
    } else if (val <= 0xFFFF) {
        out.push_back(major | 25);
        out.push_back((uint8_t)(val >> 8));
        out.push_back((uint8_t)val);
    } else if (val <= 0xFFFFFFFFULL) {
        out.push_back(major | 26);
        out.push_back((uint8_t)(val >> 24));
        out.push_back((uint8_t)(val >> 16));
        out.push_back((uint8_t)(val >> 8));
        out.push_back((uint8_t)val);
    } else {
        out.push_back(major | 27);
        for (int i = 7; i >= 0; i--) out.push_back((uint8_t)(val >> (8*i)));
    }
}

static void encodeCbor(const CborValue &val, std::vector<uint8_t> &out) {
    switch (val.type) {
    case CborValue::T_UINT:
        writeHead(out, 0, val.uint_val);
        break;
    case CborValue::T_NEGINT:
        writeHead(out, 1, val.uint_val);
        break;
    case CborValue::T_BYTES:
        writeHead(out, 2, val.bytes_val.size());
        out.insert(out.end(), val.bytes_val.begin(), val.bytes_val.end());
        break;
    case CborValue::T_STRING:
        writeHead(out, 3, val.str_val.size());
        out.insert(out.end(), val.str_val.begin(), val.str_val.end());
        break;
    case CborValue::T_ARRAY:
        writeHead(out, 4, val.arr_val.size());
        for (size_t i = 0; i < val.arr_val.size(); i++)
            encodeCbor(val.arr_val[i], out);
        break;
    case CborValue::T_MAP:
        writeHead(out, 5, val.map_val.size());
        for (size_t i = 0; i < val.map_val.size(); i++) {
            encodeCbor(val.map_val[i].first, out);
            encodeCbor(val.map_val[i].second, out);
        }
        break;
    case CborValue::T_TAG:
        writeHead(out, 6, val.uint_val);
        if (!val.arr_val.empty())
            encodeCbor(val.arr_val[0], out);
        break;
    case CborValue::T_BOOL:
        out.push_back(val.bool_val ? 0xF5 : 0xF4);
        break;
    case CborValue::T_NULL:
        out.push_back(0xF6);
        break;
    case CborValue::T_UNDEFINED:
        out.push_back(0xF7);
        break;
    case CborValue::T_FLOAT: {
        float f = (float)val.float_val;
        uint32_t bits;
        memcpy(&bits, &f, sizeof(bits));
        out.push_back(0xFA); // major 7, ai 26
        out.push_back((uint8_t)(bits >> 24));
        out.push_back((uint8_t)(bits >> 16));
        out.push_back((uint8_t)(bits >> 8));
        out.push_back((uint8_t)bits);
        break;
    }
    case CborValue::T_DOUBLE: {
        uint64_t bits;
        memcpy(&bits, &val.float_val, sizeof(bits));
        out.push_back(0xFB); // major 7, ai 27
        for (int i = 7; i >= 0; i--) out.push_back((uint8_t)(bits >> (8*i)));
        break;
    }
    }
}

// ====================================================================
// Merge logic — tree-level merge like Python cbor2
// ====================================================================
static bool isSkipKey(const std::string &key) {
    static const char *skip[] = {
        "fileType", "presetAuthor", "presetDescription", "presetName",
        "arpBankDisplayName", "clipBankDisplayName", NULL
    };
    for (const char **s = skip; *s; s++) {
        if (key == *s) return true;
    }
    return false;
}

// Merge preset CBOR tree into getState CBOR tree.
// Both are top-level maps: { moduleName: { "plainParams": {...}, ...otherKeys... }, ... }
// For each module in preset: merge into getState copy.
// plainParams: if both maps, merge param-by-param; if preset says "default", keep getState.
// Non-plainParams keys: replace entirely with preset value.
static std::vector<uint8_t> mergeCbor(const uint8_t *gsData, size_t gsLen,
                                       const uint8_t *prData, size_t prLen) {
    size_t gsPos = 0, prPos = 0;
    CborValue gsTree = decodeCbor(gsData, gsLen, gsPos);
    CborValue prTree = decodeCbor(prData, prLen, prPos);

    if (!gsTree.isMap() || !prTree.isMap()) {
        // Not both maps — return getState as-is
        return std::vector<uint8_t>(gsData, gsData + gsLen);
    }

    // Deep copy getState tree (CborValue is value-type, this copies everything)
    CborValue merged = gsTree;

    // For each module in preset, overlay onto merged tree
    for (size_t pi = 0; pi < prTree.map_val.size(); pi++) {
        const CborValue &pKey = prTree.map_val[pi].first;
        const CborValue &pVal = prTree.map_val[pi].second;

        if (!pKey.isString()) continue;
        const std::string &modName = pKey.str_val;

        // Skip preset metadata keys
        if (isSkipKey(modName)) continue;

        // Find matching module in merged tree
        CborValue *gsMod = merged.mapFind(modName);

        if (!gsMod) {
            // Module only in preset — add to merged
            merged.mapSet(modName, pVal);
            continue;
        }

        // Both exist — merge at module level
        if (gsMod->isMap() && pVal.isMap()) {
            // Iterate preset module's sub-keys
            for (size_t mi = 0; mi < pVal.map_val.size(); mi++) {
                const CborValue &subKey = pVal.map_val[mi].first;
                const CborValue &subVal = pVal.map_val[mi].second;

                if (!subKey.isString()) continue;
                const std::string &subName = subKey.str_val;

                if (subName == "plainParams") {
                    // Deep merge plainParams
                    CborValue *gsParams = gsMod->mapFind("plainParams");
                    if (gsParams && gsParams->isMap() && subVal.isMap()) {
                        // Both are maps — merge param by param
                        for (size_t pk = 0; pk < subVal.map_val.size(); pk++) {
                            const CborValue &paramKey = subVal.map_val[pk].first;
                            const CborValue &paramVal = subVal.map_val[pk].second;
                            if (paramKey.isString())
                                gsParams->mapSet(paramKey.str_val, paramVal);
                        }
                    } else if (gsParams && gsParams->isMap() &&
                               subVal.isString() && subVal.str_val == "default") {
                        // Preset says "default" — keep getState values (no-op)
                    } else if (gsParams && gsParams->isString() && subVal.isMap()) {
                        // getState says "default", preset has actual params — use preset
                        *gsParams = subVal;
                    } else {
                        // Fallback: replace with preset value
                        gsMod->mapSet("plainParams", subVal);
                    }
                } else {
                    // Non-plainParams key — replace entirely with preset value
                    gsMod->mapSet(subName, subVal);
                }
            }
        } else {
            // Not both maps — replace entirely with preset value
            *gsMod = pVal;
        }
    }

    // Encode merged tree back to CBOR bytes
    std::vector<uint8_t> out;
    encodeCbor(merged, out);
    return out;
}

} // namespace CborMerge

// loadPresetFromFile: read a preset file and feed it to IComponent::setState
// ====================================================================
bool VST3Instrument::loadPresetFromFile(const std::string &filePath, int headerSkipBytes) {
    if (!component_) return false;

    // Use POSIX I/O to avoid the project's fopen macro redirect
    int fd = open(filePath.c_str(), O_RDONLY);
    if (fd < 0) {
        Trace::Error("VST3: Cannot open preset file: %s", filePath.c_str());
        return false;
    }

    off_t fileSize = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);

    if (fileSize <= headerSkipBytes) {
        Trace::Error("VST3: Preset file too small (%ld bytes): %s", (long)fileSize, filePath.c_str());
        close(fd);
        return false;
    }

    // Skip the header (e.g. 60-byte FXP header for Surge)
    if (headerSkipBytes > 0) {
        lseek(fd, headerSkipBytes, SEEK_SET);
    }

    long dataSize = (long)fileSize - headerSkipBytes;
    std::vector<uint8_t> data(dataSize);
    ssize_t bytesRead = read(fd, &data[0], dataSize);
    close(fd);

    if (bytesRead != dataSize) {
        Trace::Error("VST3: Short read on preset file: %s", filePath.c_str());
        return false;
    }

    // If the data starts with a zlib header (0x78), it may be compressed
    // (e.g. Serum 1/2 .fxp presets use zlib-compressed chunks).
    // Try feeding the decompressed data first; if setState rejects it,
    // fall back to the raw (compressed) data.
    std::vector<uint8_t> decompressed;
    bool isZlib = (data.size() >= 2 && data[0] == 0x78);
    if (isZlib) {
        // Estimate decompressed size (try 10x, grow if needed)
        uLongf destLen = (uLongf)data.size() * 10;
        decompressed.resize(destLen);
        int zret = uncompress(&decompressed[0], &destLen,
                              &data[0], (uLong)data.size());
        if (zret == Z_BUF_ERROR) {
            // Buffer too small — try larger
            destLen = (uLongf)data.size() * 50;
            decompressed.resize(destLen);
            zret = uncompress(&decompressed[0], &destLen,
                              &data[0], (uLong)data.size());
        }
        if (zret == Z_OK) {
            decompressed.resize(destLen);
            Trace::Log("VST3", "Decompressed preset: %ld -> %ld bytes",
                (long)data.size(), (long)destLen);
        } else {
            Trace::Log("VST3", "Zlib decompression failed (ret=%d), using raw data", zret);
            isZlib = false;
            decompressed.clear();
        }
    }

    // XferJson format handler (Serum 2 .SerumPreset files)
    // ================================================
    // Both getState() and .SerumPreset use XferJson: header + JSON + binary.
    // Binary = 4-byte LE decompressed_size + 4-byte LE version + zstd data.
    // The zstd data decompresses to CBOR (map of modules with parameters).
    // getState CBOR has ALL parameters; preset CBOR has only CHANGED ones.
    // We merge preset values into getState CBOR, re-encode, and setState.
    bool isXferJson = (data.size() > 17 &&
                       memcmp(&data[0], "XferJson\0", 9) == 0);
    if (isXferJson) {
        IComponent *comp = (IComponent *)component_;

        // 1. Get current state from the plugin
        SimpleMemoryStream gsStream;
        tresult gsRes = comp->getState(&gsStream);
        if (gsRes != kResultOk || gsStream.size() < 17) {
            Trace::Error("VST3: getState failed for CBOR merge (res=%d)", (int)gsRes);
            return false;
        }
        const uint8_t *gsRaw = (const uint8_t *)gsStream.data();
        size_t gsRawLen = gsStream.size();

        // 2. Parse XferJson headers from both getState and preset
        if (memcmp(gsRaw, "XferJson\0", 9) != 0) {
            Trace::Error("VST3: getState is not XferJson format");
            return false;
        }
        uint64_t gsJsonLen = 0;
        for (int i = 0; i < 8; i++) gsJsonLen |= ((uint64_t)gsRaw[9+i]) << (8*i);
        size_t gsBinaryOff = 17 + (size_t)gsJsonLen;
        if (gsBinaryOff + 8 >= gsRawLen) {
            Trace::Error("VST3: getState XferJson too short");
            return false;
        }

        uint64_t prJsonLen = 0;
        for (int i = 0; i < 8; i++) prJsonLen |= ((uint64_t)data[9+i]) << (8*i);
        size_t prBinaryOff = 17 + (size_t)prJsonLen;
        if (prBinaryOff + 8 >= data.size()) {
            Trace::Error("VST3: preset XferJson too short");
            return false;
        }

        // 3. Parse binary sub-headers (4-byte LE decompressed size + 4-byte LE version)
        uint32_t gsDecompSz = 0, gsVersion = 0;
        memcpy(&gsDecompSz, &gsRaw[gsBinaryOff], 4);
        memcpy(&gsVersion, &gsRaw[gsBinaryOff + 4], 4);

        uint32_t prDecompSz = 0, prVersion = 0;
        memcpy(&prDecompSz, &data[prBinaryOff], 4);
        memcpy(&prVersion, &data[prBinaryOff + 4], 4);

        // 4. Decompress both zstd blobs
        const uint8_t *gsZstd = &gsRaw[gsBinaryOff + 8];
        size_t gsZstdLen = gsRawLen - gsBinaryOff - 8;
        std::vector<uint8_t> gsCbor(gsDecompSz);
        size_t gsActual = ZSTD_decompress(&gsCbor[0], gsDecompSz, gsZstd, gsZstdLen);
        if (ZSTD_isError(gsActual)) {
            Trace::Error("VST3: zstd decompress getState failed: %s", ZSTD_getErrorName(gsActual));
            return false;
        }
        gsCbor.resize(gsActual);

        const uint8_t *prZstd = &data[prBinaryOff + 8];
        size_t prZstdLen = data.size() - prBinaryOff - 8;
        std::vector<uint8_t> prCbor(prDecompSz);
        size_t prActual = ZSTD_decompress(&prCbor[0], prDecompSz, prZstd, prZstdLen);
        if (ZSTD_isError(prActual)) {
            Trace::Error("VST3: zstd decompress preset failed: %s", ZSTD_getErrorName(prActual));
            return false;
        }
        prCbor.resize(prActual);

        Trace::Log("VST3", "CBOR merge: getState=%ld bytes, preset=%ld bytes",
            (long)gsActual, (long)prActual);

        // 5. Merge preset CBOR into getState CBOR
        std::vector<uint8_t> merged = CborMerge::mergeCbor(
            &gsCbor[0], gsCbor.size(), &prCbor[0], prCbor.size());
        Trace::Log("VST3", "CBOR merged: %ld bytes", (long)merged.size());

        // 6. Compress merged CBOR with zstd
        size_t compBound = ZSTD_compressBound(merged.size());
        std::vector<uint8_t> compBuf(compBound);
        size_t compSz = ZSTD_compress(&compBuf[0], compBound,
                                       &merged[0], merged.size(), 19);
        if (ZSTD_isError(compSz)) {
            Trace::Error("VST3: zstd compress merged failed: %s", ZSTD_getErrorName(compSz));
            return false;
        }
        compBuf.resize(compSz);

        // 7. Compute MD5 of compressed data and update JSON hash
        std::string compMd5 = CborMerge::md5Hex(&compBuf[0], compSz);
        std::string gsJson((const char*)gsRaw + 17, (size_t)gsJsonLen);
        std::string updatedJson = CborMerge::updateJsonHash(gsJson, compMd5);
        Trace::Log("VST3", "XferJson hash updated: %s", compMd5.c_str());

        // 8. Reassemble XferJson with updated JSON and merged binary
        uint32_t mergedDecompSz = (uint32_t)merged.size();
        uint64_t newJsonLen = (uint64_t)updatedJson.size();
        std::vector<uint8_t> output;
        output.reserve(9 + 8 + (size_t)newJsonLen + 8 + compSz);
        // XferJson\0
        output.insert(output.end(), gsRaw, gsRaw + 9);
        // JSON length (8 bytes LE)
        for (int i = 0; i < 8; i++)
            output.push_back((uint8_t)(newJsonLen >> (8*i)));
        // JSON body (with updated hash)
        output.insert(output.end(), updatedJson.begin(), updatedJson.end());
        // Binary sub-header: decompressed size (4 LE) + version (4 LE)
        output.push_back(mergedDecompSz & 0xFF);
        output.push_back((mergedDecompSz >> 8) & 0xFF);
        output.push_back((mergedDecompSz >> 16) & 0xFF);
        output.push_back((mergedDecompSz >> 24) & 0xFF);
        output.push_back(gsVersion & 0xFF);
        output.push_back((gsVersion >> 8) & 0xFF);
        output.push_back((gsVersion >> 16) & 0xFF);
        output.push_back((gsVersion >> 24) & 0xFF);
        // Compressed CBOR
        output.insert(output.end(), compBuf.begin(), compBuf.end());

        Trace::Log("VST3", "XferJson assembled: %ld bytes (JSON=%ld, binary=%ld+%ld)",
            (long)output.size(), (long)newJsonLen, (long)8, (long)compSz);

        // 9. Feed to setState (with timeout to survive Wine stack overflows)
        auto stateStreamPtr = std::make_shared<SimpleMemoryStream>();
        stateStreamPtr->setData(&output[0], output.size());
        {
            auto promisePtr = std::make_shared<std::promise<tresult>>();
            auto future = promisePtr->get_future();
            std::thread t([comp, stateStreamPtr, promisePtr]() {
                tresult r = comp->setState(stateStreamPtr.get());
                promisePtr->set_value(r);
            });
            auto status = future.wait_for(std::chrono::seconds(15));
            if (status == std::future_status::timeout) {
                t.detach();
                Trace::Error("VST3: setState timed out after 15s (probable Wine stack overflow), skipping preset");
                return false;
            }
            t.join();
            tresult setRes = future.get();
            if (setRes != kResultOk) {
                Trace::Error("VST3: setState failed with merged CBOR (res=%d)", (int)setRes);
                return false;
            }
        }
        Trace::Log("VST3", "setState succeeded with merged CBOR preset");

        // Sync controller
        if (editController_ && !componentIsController_) {
            IEditController *ctrl = (IEditController *)editController_;
            stateStreamPtr->resetRead();
            ctrl->setComponentState(stateStreamPtr.get());
        }
        // Sync parameter values back into our Variables
        if (editController_) {
            IEditController *ctrl = (IEditController *)editController_;
            for (size_t i = 0; i < parameters_.size(); ++i) {
                VST3PluginParameter &p = parameters_[i];
                double norm = ctrl->getParamNormalized(p.paramId);
                p.currentValue = norm;
                if (p.variable) {
                    int maxVal = (p.stepCount > 0 && p.stepCount <= 255) ? p.stepCount : 255;
                    int scaled = (int)(norm * maxVal + 0.5);
                    if (scaled < 0) scaled = 0;
                    if (scaled > maxVal) scaled = maxVal;
                    p.variable->SetInt(scaled, false);
                }
            }
        }
        Trace::Log("VST3", "Loaded preset from file: %s", filePath.c_str());
        return true;
    }

    // Non-XferJson fallback path (for other plugins like Surge)
    // Feed the data to IComponent::setState via our SimpleMemoryStream.
    IComponent *comp = (IComponent *)component_;
    SimpleMemoryStream stream;
    bool loaded = false;

    // Try zlib-decompressed data (for FXP files with compressed chunks)
    if (!loaded && isZlib && !decompressed.empty()) {
        stream.setData(&decompressed[0], decompressed.size());
        tresult res = comp->setState(&stream);
        if (res == kResultOk) {
            Trace::Log("VST3", "setState succeeded with decompressed data");
            loaded = true;
        } else {
            Trace::Log("VST3", "setState rejected decompressed data (res=%d)", (int)res);
        }
    }

    // Fall back to raw data as-is
    if (!loaded) {
        stream.setData(&data[0], data.size());
        tresult res = comp->setState(&stream);
        if (res != kResultOk) {
            Trace::Error("VST3: setState failed for preset file: %s (result=%d)",
                filePath.c_str(), (int)res);
            return false;
        }
        Trace::Log("VST3", "setState succeeded with raw data");
    }

    // Also notify the controller about the component state change
    if (editController_ && !componentIsController_) {
        IEditController *ctrl = (IEditController *)editController_;
        stream.resetRead();
        ctrl->setComponentState(&stream);
    }

    // Sync parameter values from the plugin back into our Variables
    if (editController_) {
        IEditController *ctrl = (IEditController *)editController_;
        for (size_t i = 0; i < parameters_.size(); ++i) {
            VST3PluginParameter &p = parameters_[i];
            double norm = ctrl->getParamNormalized(p.paramId);
            p.currentValue = norm;
            if (p.variable) {
                int maxVal = (p.stepCount > 0 && p.stepCount <= 255) ? p.stepCount : 255;
                int scaled = (int)(norm * maxVal + 0.5);
                if (scaled < 0) scaled = 0;
                if (scaled > maxVal) scaled = maxVal;
                p.variable->SetInt(scaled, false);
            }
        }
    }

    Trace::Log("VST3", "Loaded preset from file: %s (%ld bytes, skipped %d)",
        filePath.c_str(), dataSize, headerSkipBytes);
    return true;
}

// ====================================================================
// savePresetToFile: save current plugin state to a native preset file
// ====================================================================
bool VST3Instrument::savePresetToFile(const std::string &filePath) {
    if (!component_) {
        Trace::Error("VST3: Cannot save preset – plugin not loaded");
        return false;
    }

    // Find the mapping for this plugin
    const VST3PluginPresetMapping *mapping = nullptr;
    for (int i = 0; knownPresetMappings[i].pluginNameSubstring != nullptr; i++) {
        if (strstr(name_, knownPresetMappings[i].pluginNameSubstring) != nullptr) {
            mapping = &knownPresetMappings[i];
            break;
        }
    }
    if (!mapping) {
        Trace::Error("VST3: No preset mapping for '%s' – cannot save", name_);
        return false;
    }

    // Get the raw state from IComponent::getState()
    IComponent *comp = (IComponent *)component_;
    SimpleMemoryStream stream;
    tresult res = comp->getState(&stream);
    if (res != kResultOk || stream.size() == 0) {
        Trace::Error("VST3: getState failed or returned empty state");
        return false;
    }

    const uint8_t *rawData = stream.data();
    size_t rawSize = stream.size();

    Trace::Log("VST3", "Got %d bytes of raw state data for save", (int)rawSize);

    // Write the file
    int fd = open(filePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        Trace::Error("VST3: Cannot create preset file: %s", filePath.c_str());
        return false;
    }

    // Determine whether to prepend an FXP header based on save extension
    // (e.g. Surge XT saves as .fxp with header; Serum saves as .SerumPreset
    //  with no header even though it also reads .fxp)
    if (mapping->headerSkipBytes > 0) {
        uint8_t fxpHeader[60];
        memset(fxpHeader, 0, 60);

        // FXP magic: "CcnK"
        fxpHeader[0] = 'C'; fxpHeader[1] = 'c'; fxpHeader[2] = 'n'; fxpHeader[3] = 'K';

        // byteSize: total file size - 8 (big-endian 32-bit)
        uint32_t byteSize = (uint32_t)(60 - 8 + rawSize);
        fxpHeader[4] = (byteSize >> 24) & 0xFF;
        fxpHeader[5] = (byteSize >> 16) & 0xFF;
        fxpHeader[6] = (byteSize >> 8) & 0xFF;
        fxpHeader[7] = byteSize & 0xFF;

        // fxMagic: "FPCh" for opaque chunk
        fxpHeader[8] = 'F'; fxpHeader[9] = 'P'; fxpHeader[10] = 'C'; fxpHeader[11] = 'h';

        // version: 1
        fxpHeader[15] = 1;

        // fxID: "csST" for Surge XT
        fxpHeader[16] = 'c'; fxpHeader[17] = 's'; fxpHeader[18] = 'S'; fxpHeader[19] = 'T';

        // fxVersion: 1
        fxpHeader[23] = 1;

        // prgName: extract from file path
        std::string fname = filePath;
        size_t slashPos = fname.rfind('/');
        if (slashPos != std::string::npos) fname = fname.substr(slashPos + 1);
        size_t dotPos = fname.rfind('.');
        if (dotPos != std::string::npos) fname = fname.substr(0, dotPos);
        strncpy((char *)&fxpHeader[28], fname.c_str(), 27);
        fxpHeader[55] = 0;

        // chunkSize at offset 56
        uint32_t chunkSize = (uint32_t)rawSize;
        fxpHeader[56] = (chunkSize >> 24) & 0xFF;
        fxpHeader[57] = (chunkSize >> 16) & 0xFF;
        fxpHeader[58] = (chunkSize >> 8) & 0xFF;
        fxpHeader[59] = chunkSize & 0xFF;

        ssize_t w = write(fd, fxpHeader, 60);
        (void)w;
    }

    // Write the raw state data
    ssize_t w = write(fd, rawData, rawSize);
    (void)w;
    close(fd);

    Trace::Log("VST3", "Saved preset to: %s (%d bytes + %d header)",
        filePath.c_str(), (int)rawSize, mapping->headerSkipBytes);
    return true;
}

// ====================================================================
// getCurrentBankDirectory: get directory path for the current bank
// ====================================================================
std::string VST3Instrument::getCurrentBankDirectory() const {
    if (!usingFilePresets_) return "";
    if (currentBank_ < 0 || currentBank_ >= (int)filePresetsByBank_.size()) return "";
    if (filePresetsByBank_[currentBank_].empty()) return "";

    const std::string &path = filePresetsByBank_[currentBank_][0].filePath;
    size_t slashPos = path.rfind('/');
    if (slashPos != std::string::npos) {
        return path.substr(0, slashPos);
    }
    return "";
}

// ====================================================================
// getPresetExtension: get native file extension for this plugin
// ====================================================================
const char *VST3Instrument::getPresetExtension() const {
    for (int i = 0; knownPresetMappings[i].pluginNameSubstring != nullptr; i++) {
        if (strstr(name_, knownPresetMappings[i].pluginNameSubstring) != nullptr) {
            return knownPresetMappings[i].extension;
        }
    }
    return nullptr;
}

// ====================================================================
// canSavePreset: check if preset saving is supported for this plugin
// ====================================================================
bool VST3Instrument::canSavePreset() const {
    if (!usingFilePresets_ || !component_) return false;
    return getPresetExtension() != nullptr;
}

// ====================================================================
// refreshPresets: re-scan preset files and update lists
// ====================================================================
void VST3Instrument::refreshPresets() {
    int savedBank = currentBank_;
    discoverPresetFiles();
    if (savedBank >= 0 && savedBank < (int)programLists_.size()) {
        currentBank_ = savedBank;
    }
    int count = GetPresetCountForBank(currentBank_);
    if (count > 0) {
        currentPreset_ = count - 1;
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

    // --- File-based preset loading ---
    if (usingFilePresets_) {
        if (currentBank_ < (int)filePresetsByBank_.size() &&
            presetIdx < (int)filePresetsByBank_[currentBank_].size()) {

            const VST3FilePreset &fp = filePresetsByBank_[currentBank_][presetIdx];

            // Determine headerSkipBytes from the actual file extension
            int headerSkip = 0;
            for (int m = 0; knownPresetMappings[m].pluginNameSubstring != nullptr; m++) {
                if (strstr(name_, knownPresetMappings[m].pluginNameSubstring) != nullptr) {
                    headerSkip = knownPresetMappings[m].headerSkipBytes;
                    // If a second extension is defined, check if this file uses it
                    if (knownPresetMappings[m].extension2) {
                        size_t ext2Len = strlen(knownPresetMappings[m].extension2);
                        if (fp.filePath.size() > ext2Len &&
                            fp.filePath.substr(fp.filePath.size() - ext2Len) ==
                                knownPresetMappings[m].extension2) {
                            headerSkip = knownPresetMappings[m].headerSkipBytes2;
                        }
                    }
                    break;
                }
            }

            if (loadPresetFromFile(fp.filePath, headerSkip)) {
                Trace::Log("VST3", "Loaded file preset %d ('%s') from bank '%s'",
                    presetIdx, fp.name.c_str(), pl.name.c_str());
            }
        }
        return;
    }

    // --- MIDI-based preset selection (gearmulator/OsTIrus) ---
    // Send bank select LSB (CC#32) + program change as kLegacyMIDICCOutEvent
    // events through the VST3 event list.  JUCE converts these to real MIDI
    // messages that the Virus emulator's Microcontroller processes.
    if (usingMidiPresets_) {
        int bankVal = currentBank_;
        if (bankVal < 0) bankVal = 0;
        if (bankVal > 127) bankVal = 127;

        int progVal = presetIdx;
        if (progVal < 0) progVal = 0;
        if (progVal > 127) progVal = 127;

        PendingMidiCC bankCC;
        bankCC.controlNumber = 32;  // kCtrlBankSelectLSB — CC#32
        bankCC.channel = 0;
        bankCC.value = (int8_t)bankVal;
        bankCC.value2 = 0;

        PendingMidiCC progCC;
        progCC.controlNumber = (uint8_t)130;  // kCtrlProgramChange
        progCC.channel = 0;
        progCC.value = (int8_t)progVal;
        progCC.value2 = 0;

        pendingEventsMutex_.Lock();
        pendingMidiCCs_.push_back(bankCC);
        pendingMidiCCs_.push_back(progCC);
        pendingEventsMutex_.Unlock();

        Trace::Log("VST3", "MIDI preset: bank %d ('%s') preset %d ('%s')",
            currentBank_, pl.name.c_str(), presetIdx,
            GetPresetName(presetIdx));
        return;
    }

    // --- IUnitInfo-based preset selection via kIsProgramChange parameter ---
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
    int32 numEventInputBuses = comp->getBusCount(kEvent, kInput);

    Trace::Log("VST3", "setupProcessing: audioIn=%d audioOut=%d eventIn=%d block=%d rate=%d",
        (int)numInputBuses, (int)numOutputBuses, (int)numEventInputBuses, bufferSize, sampleRate);

    if (numOutputBuses > 0) {
        // Activate output bus 0
        tresult actRes = comp->activateBus(kAudio, kOutput, 0, true);
        Trace::Log("VST3", "  activateBus(audioOut,0) = %d", (int)actRes);

        if (numInputBuses > 0) {
            comp->activateBus(kAudio, kInput, 0, true);
            SpeakerArrangement inputs[1] = { stereo };
            SpeakerArrangement outputs[1] = { stereo };
            tresult busRes = proc->setBusArrangements(inputs, 1, outputs, 1);
            Trace::Log("VST3", "  setBusArrangements(in=1,out=1) = %d", (int)busRes);
        } else {
            SpeakerArrangement outputs[1] = { stereo };
            tresult busRes = proc->setBusArrangements(nullptr, 0, outputs, 1);
            Trace::Log("VST3", "  setBusArrangements(in=0,out=1) = %d", (int)busRes);
        }
    }

    // Activate event (MIDI) buses
    for (int32 i = 0; i < numEventInputBuses; i++) {
        tresult evtRes = comp->activateBus(kEvent, kInput, i, true);
        Trace::Log("VST3", "  activateBus(eventIn,%d) = %d", (int)i, (int)evtRes);
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
// Try to parse a .vst3 bundle's Contents/moduleinfo.json (VST 3.7.5+) to get
// plugin class info without dlopen'ing the module. This is essential for
// yabridge-bridged bundles where dlopen would trigger a Wine launch.
// Returns true if moduleinfo.json was found (even if no matching classes).
static bool tryParseModuleInfo(const std::string& bundlePath,
                               const char* filterCategory,
                               std::vector<VST3PluginInfo>& result) {
    std::string jsonPath = bundlePath + "/Contents/moduleinfo.json";
    struct stat st;
    if (stat(jsonPath.c_str(), &st) != 0 || st.st_size <= 0)
        return false;

    FILE* f = fopen(jsonPath.c_str(), "r");
    if (!f) return false;
    std::string json;
    json.resize(st.st_size);
    size_t nread = fread(&json[0], 1, st.st_size, f);
    fclose(f);
    if ((off_t)nread != st.st_size) return false;

    // Simple JSON field extractor for a known-flat object
    auto extractString = [](const std::string& obj,
                            const char* key) -> std::string {
        std::string needle = std::string("\"")
                             + key + "\"";
        size_t kpos = obj.find(needle);
        if (kpos == std::string::npos) return "";
        size_t colon = obj.find(':', kpos + needle.size());
        if (colon == std::string::npos) return "";
        size_t qstart = obj.find('"', colon + 1);
        if (qstart == std::string::npos) return "";
        size_t qend = obj.find('"', qstart + 1);
        if (qend == std::string::npos) return "";
        return obj.substr(qstart + 1, qend - qstart - 1);
    };

    // Find "Classes" array
    size_t classesPos = json.find("\"Classes\"");
    if (classesPos == std::string::npos) return true; // file found, no classes

    size_t arrStart = json.find('[', classesPos);
    if (arrStart == std::string::npos) return true;

    // Iterate {…} objects inside the array
    size_t pos = arrStart + 1;
    while (pos < json.size()) {
        size_t objStart = json.find('{', pos);
        if (objStart == std::string::npos) break;
        // Check we haven't left the Classes array
        size_t arrEnd = json.find(']', arrStart);
        if (arrEnd != std::string::npos && objStart > arrEnd) break;

        size_t objEnd = objStart + 1;
        int depth = 1;
        while (objEnd < json.size() && depth > 0) {
            if (json[objEnd] == '{') depth++;
            else if (json[objEnd] == '}') depth--;
            if (depth > 0) objEnd++;
        }
        if (depth != 0) break;

        std::string obj = json.substr(objStart, objEnd - objStart + 1);

        std::string cid  = extractString(obj, "CID");
        std::string cat  = extractString(obj, "Category");
        std::string name = extractString(obj, "Name");
        std::string sub  = extractString(obj, "Sub Categories");

        if (cat == "Audio Module Class" && !cid.empty() && !name.empty()) {
            bool matches = false;
            if (strcmp(filterCategory, "Instrument") == 0) {
                matches = sub.find("Instrument") != std::string::npos;
            } else if (strcmp(filterCategory, "Fx") == 0) {
                matches = sub.find("Fx") != std::string::npos
                          && sub.find("Instrument") == std::string::npos;
            }
            // If no subcategories at all, include as fallback
            if (sub.empty()) matches = true;

            if (matches) {
                if (name.find("Steam") == std::string::npos &&
                    name.find("Valve") == std::string::npos) {
                    VST3PluginInfo info;
                    info.name = name;
                    info.path = bundlePath;
                    info.classIdStr = cid;
                    hexToTuid(cid, info.classId);
                    result.push_back(info);
                }
            }
        }
        pos = objEnd + 1;
    }
    return true; // moduleinfo.json was found and handled
}

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
        // Try moduleinfo.json first (fast, no loading / Wine needed)
        if (tryParseModuleInfo(bundlePath, "Instrument", result)) {
            continue;
        }

        // Skip yabridge bridge bundles that lack moduleinfo.json — dlopen'ing
        // them would trigger the chainloader which needs Wine and shows a
        // desktop error notification if libyabridge-vst3.so isn't reachable.
        if (bundlePath.find("/yabridge/") != std::string::npos) {
            continue;
        }

        // Fall back to dlopen for native plugins without moduleinfo.json
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

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
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

// Declared in LINUXMain.cpp — set to non-zero to make SIGABRT survivable
// when a Wine host process is known to be stuck/dead.
extern volatile sig_atomic_t g_vst3ComponentDead;
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
    componentDead_ = false;
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
        // If a previous setState timed out, the Wine process is stuck.
        // Don't attempt any IPC — just output silence forever.
        if (componentDead_) {
            if (cachedOutputBuffer_ && cachedOutputSize_ >= size) {
                memset(cachedOutputBuffer_, 0, size * 2 * sizeof(fixed));
            }
        } else
        // TryLock the component mutex — if a preset is being loaded on the
        // UI thread (which holds the lock), skip this cycle with silence
        // rather than blocking the audio thread.
        if (!componentMutex_.TryLock()) {
            // Preset loading in progress — output silence
            if (cachedOutputBuffer_ && cachedOutputSize_ >= size) {
                memset(cachedOutputBuffer_, 0, size * 2 * sizeof(fixed));
            }
        } else {
        // componentMutex_ is held — safe to use the VST3 component

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
        (void)processResult;
        float* postProcL = outputBus.channelBuffers32[0];
        float* postProcR = outputBus.channelBuffers32[1];

        // If yabridge replaced the output buffer pointers, read from the new ones
        if (postProcL != preProcL || postProcR != preProcR) {
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
        componentMutex_.Unlock();
        } // end TryLock succeeded
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

std::string VST3Instrument::GetComponentStateBase64() {
    if (!component_ || componentDead_) return "";
    IComponent* comp = (IComponent*)component_;

    // getState() goes through yabridge IPC and can hang — run with timeout
    auto promise = std::make_shared<std::promise<std::string>>();
    auto future = promise->get_future();

    componentMutex_.Lock();
    std::thread t([comp, promise]() {
        try {
            SimpleMemoryStream stream;
            tresult res = comp->getState(&stream);
            if (res == kResultOk && stream.size() > 0) {
                promise->set_value(base64Encode(stream.data(), stream.size()));
            } else {
                promise->set_value("");
            }
        } catch (...) {
            try { promise->set_value(""); } catch (...) {}
        }
    });

    std::string result;
    auto status = future.wait_for(std::chrono::seconds(10));
    if (status == std::future_status::timeout) {
        t.detach();
        componentDead_ = true;
        g_vst3ComponentDead = 1;
        Trace::Error("VST3: getState timed out during save — plugin is now DEAD");
        componentMutex_.Unlock();
        return "";
    }
    t.join();
    result = future.get();
    componentMutex_.Unlock();
    return result;
}

std::string VST3Instrument::GetControllerStateBase64() {
    if (!editController_ || componentDead_) return "";
    IEditController* ctrl = (IEditController*)editController_;

    // getState() goes through yabridge IPC and can hang — run with timeout
    auto promise = std::make_shared<std::promise<std::string>>();
    auto future = promise->get_future();

    componentMutex_.Lock();
    std::thread t([ctrl, promise]() {
        try {
            SimpleMemoryStream stream;
            tresult res = ctrl->getState(&stream);
            if (res == kResultOk && stream.size() > 0) {
                promise->set_value(base64Encode(stream.data(), stream.size()));
            } else {
                promise->set_value("");
            }
        } catch (...) {
            try { promise->set_value(""); } catch (...) {}
        }
    });

    std::string result;
    auto status = future.wait_for(std::chrono::seconds(10));
    if (status == std::future_status::timeout) {
        t.detach();
        componentDead_ = true;
        g_vst3ComponentDead = 1;
        Trace::Error("VST3: ctrl getState timed out during save — plugin is now DEAD");
        componentMutex_.Unlock();
        return "";
    }
    t.join();
    result = future.get();
    componentMutex_.Unlock();
    return result;
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
        return false;
    }

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

    // Detect yabridge-bridged plugins (the .so is a symlink to the
    // yabridge chainloader).
    bool isYabridge = false;
    {
        char linkTarget[512];
        ssize_t len = readlink(soPath.c_str(), linkTarget, sizeof(linkTarget) - 1);
        if (len > 0) {
            linkTarget[len] = '\0';
            if (strstr(linkTarget, "yabridge"))
                isYabridge = true;
        }
        if (soPath.find("yabridge") != std::string::npos)
            isYabridge = true;
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

    // For yabridge plugins, ModuleEntry spawns a Wine host process.
    // If that process crashes or hangs, it can take our whole app down.
    // Use fork() to probe ModuleEntry in a child process first —
    // if the child survives, we know it's safe to call in the parent.
    if (isYabridge) {
        dlclose(moduleHandle_);
        moduleHandle_ = nullptr;

        pid_t pid = fork();
        if (pid < 0) {
            Trace::Error("VST3: fork() failed: %s", strerror(errno));
            return;
        }
        if (pid == 0) {
            // ---- Child process ----
            void *childHandle = dlopen(soPath.c_str(), RTLD_LAZY);
            if (!childHandle) _exit(10);
            VST3ModuleEntryFunc childEntry =
                (VST3ModuleEntryFunc)dlsym(childHandle, "ModuleEntry");
            if (!childEntry) { dlclose(childHandle); _exit(11); }
            bool ok = childEntry(childHandle);
            if (!ok) { dlclose(childHandle); _exit(12); }
            VST3ModuleExitFunc childExit =
                (VST3ModuleExitFunc)dlsym(childHandle, "ModuleExit");
            if (childExit) childExit();
            dlclose(childHandle);
            _exit(0);
        }

        // ---- Parent process: wait for child with timeout ----
        int status = 0;
        bool childDone = false;
        for (int i = 0; i < 60; i++) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w > 0) { childDone = true; break; }
            if (w < 0) { childDone = true; break; }
            usleep(500000); // 0.5s
        }
        if (!childDone) {
            Trace::Error("VST3: ModuleEntry probe timed out — killing child");
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return;
        }

        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            if (code != 0) {
                if (code >= 128) {
                    Trace::Error("VST3: ModuleEntry probe crashed (signal %d)",
                                 code - 128);
                } else {
                    Trace::Error("VST3: ModuleEntry probe failed (code %d)",
                                 code);
                }
                return;
            }
        } else if (WIFSIGNALED(status)) {
            Trace::Error("VST3: ModuleEntry probe killed by signal %d",
                         WTERMSIG(status));
            return;
        } else {
            Trace::Error("VST3: ModuleEntry probe failed (status=0x%x)",
                         status);
            return;
        }

        // Probe passed — re-dlopen + ModuleEntry in the parent
        moduleHandle_ = dlopen(soPath.c_str(), RTLD_LAZY);
        if (!moduleHandle_) {
            Trace::Error("VST3: re-dlopen failed: %s", dlerror());
            return;
        }
        moduleEntry = (VST3ModuleEntryFunc)dlsym(moduleHandle_, "ModuleEntry");
        if (!moduleEntry) {
            Trace::Error("VST3: ModuleEntry missing on re-dlopen");
            dlclose(moduleHandle_);
            moduleHandle_ = nullptr;
            return;
        }
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
                    // Also grab subcategories so we can fix moduleinfo.json
                    if (factory2) {
                        PClassInfo2 ci2;
                        if (factory2->getClassInfo2(i, &ci2) == kResultOk) {
                            resolvedSub = ci2.subCategories;
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
    } else {
        // Create separate controller
        TUID controllerCID;
        res = component->getControllerClassId(controllerCID);
        if (res == kResultOk) {
            res = factory->createInstance(
                controllerCID,
                IEditController::iid,
                (void**)&controller
            );
            if (res == kResultOk && controller) {
                controller->initialize(static_cast<FUnknown*>(&gHostApp));
                editController_ = controller;
                componentIsController_ = false;
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
    // u-he Diva: .h2p text preset files, fed directly to setState.
    // u-he plugins recognise the .h2p format via setState.
    {
        "Diva",
        {
            nullptr  // Directories resolved at runtime from WINEPREFIX
        },
        ".h2p",
        0,   // No header skip — text format fed as-is
        nullptr, 0
    },
    // Fors Pivot: .pivot XML preset files, fed directly to setState.
    {
        "Pivot",
        {
            nullptr  // Directories resolved at runtime from $HOME
        },
        ".pivot",
        0,   // No header skip — XML state fed as-is
        nullptr, 0
    },
    // TAL-NoiseMaker: .noisemakerpreset XML files, JUCE-wrapped before setState.
    {
        "NoiseMaker",
        {
            nullptr  // Directories resolved at runtime from WINEPREFIX
        },
        ".noisemakerpreset",
        0,   // No header skip — raw XML wrapped via JUCE binary format
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
            }
            break;
        }
    }

    // Try to get IUnitInfo from the edit controller
    IUnitInfo* unitInfo = nullptr;
    tresult res = ctrl->queryInterface(IUnitInfo::iid, (void**)&unitInfo);
    if (res != kResultOk || !unitInfo) {
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

        programLists_.push_back(pl);
    }

    unitInfo->release();

    if (!programLists_.empty()) {
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
        return;
    }

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
        } else if (strstr(name_, "Diva") != nullptr) {
            // u-he Diva presets live inside the Wine prefix.
            std::string pfxBase;
            const char *wpEnv = getenv("WINEPREFIX");
            if (wpEnv && wpEnv[0]) {
                pfxBase = wpEnv;
            } else {
                pfxBase = homeDir + "/Documents/SDTracker/wineprefix";
            }
            const char *userNames[] = { "steamuser", nullptr };
            const char *prefixes[] = { "", "/pfx", nullptr };
            // Also try the host $USER in case of the symlink
            const char *hostUser = getenv("USER");
            for (int pi = 0; prefixes[pi] != nullptr; pi++) {
                for (int ui = 0; userNames[ui] != nullptr; ui++) {
                    std::string base = pfxBase + prefixes[pi]
                        + "/drive_c/users/" + userNames[ui]
                        + "/Documents/u-he/Diva.data";
                    scanDirs.push_back(base + "/Presets/Diva");
                    scanDirs.push_back(base + "/UserPresets/Diva");
                }
                if (hostUser && hostUser[0] &&
                    strcmp(hostUser, "steamuser") != 0) {
                    std::string base = pfxBase + prefixes[pi]
                        + "/drive_c/users/" + hostUser
                        + "/Documents/u-he/Diva.data";
                    scanDirs.push_back(base + "/Presets/Diva");
                    scanDirs.push_back(base + "/UserPresets/Diva");
                }
            }
        } else if (strstr(name_, "Pivot") != nullptr) {
            // Fors Pivot: presets at ~/.config/Fors/Pivot/Presets
            scanDirs.push_back(homeDir + "/.config/Fors/Pivot/Presets");
            scanDirs.push_back(homeDir + "/.config/fors/pivot/presets");
        } else if (strstr(name_, "NoiseMaker") != nullptr) {
            // TAL-NoiseMaker presets live inside the Wine prefix.
            std::string pfxBase;
            const char *wpEnv = getenv("WINEPREFIX");
            if (wpEnv && wpEnv[0]) {
                pfxBase = wpEnv;
            } else {
                pfxBase = homeDir + "/Documents/SDTracker/wineprefix";
            }
            const char *userNames[] = { "steamuser", nullptr };
            const char *prefixes[] = { "", "/pfx", nullptr };
            const char *hostUser = getenv("USER");
            for (int pi = 0; prefixes[pi] != nullptr; pi++) {
                for (int ui = 0; userNames[ui] != nullptr; ui++) {
                    std::string base = pfxBase + prefixes[pi]
                        + "/drive_c/users/" + userNames[ui]
                        + "/AppData/Roaming/ToguAudioLine/TAL-NoiseMaker/presets";
                    scanDirs.push_back(base + "/Factory Presets");
                    scanDirs.push_back(base);
                }
                if (hostUser && hostUser[0] &&
                    strcmp(hostUser, "steamuser") != 0) {
                    std::string base = pfxBase + prefixes[pi]
                        + "/drive_c/users/" + hostUser
                        + "/AppData/Roaming/ToguAudioLine/TAL-NoiseMaker/presets";
                    scanDirs.push_back(base + "/Factory Presets");
                    scanDirs.push_back(base);
                }
            }
        }
    }
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
}

// ====================================================================
// discoverHardcodedPresets: use built-in ROM patch names for OsTIrus
// ====================================================================
void VST3Instrument::discoverHardcodedPresets() {
    if (strstr(name_, "OsTIrus") == nullptr) return;

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

    // Open with POSIX I/O (avoid project fopen macro)
    int fd = open(cachePath.c_str(), O_RDONLY);
    if (fd < 0) {
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
        return;
    }

    // Presets will be selected by sending MIDI bank select + program change
    // via kLegacyMIDICCOutEvent in the VST3 event list. JUCE (used by gearmulator)
    // converts these to real MIDI messages that the Virus emulator processes.
    usingMidiPresets_ = true;

    currentBank_ = 0;
    currentPreset_ = 0;
}

// ---- Binary-safe CBOR merge utilities (no value decode/re-encode) ----
namespace {

// Minimal MD5 implementation (RFC 1321) — produces 32-char hex digest.
struct MD5 {
    uint32_t a0,b0,c0,d0;
    uint64_t totalLen;
    uint8_t buf[64];
    size_t bufLen;

    static const uint32_t S[64];
    static const uint32_t K[64];

    MD5() : a0(0x67452301), b0(0xefcdab89), c0(0x98badcfe), d0(0x10325476),
            totalLen(0), bufLen(0) {}

    void update(const uint8_t *data, size_t len) {
        totalLen += len;
        size_t i = 0;
        if (bufLen > 0) {
            size_t fill = 64 - bufLen;
            if (len < fill) { memcpy(buf + bufLen, data, len); bufLen += len; return; }
            memcpy(buf + bufLen, data, fill);
            transform(buf);
            i = fill; bufLen = 0;
        }
        for (; i + 64 <= len; i += 64) transform(data + i);
        if (i < len) { memcpy(buf, data + i, len - i); bufLen = len - i; }
    }

    void transform(const uint8_t *block) {
        uint32_t M[16];
        for (int j = 0; j < 16; j++)
            M[j] = (uint32_t)block[j*4] | ((uint32_t)block[j*4+1]<<8) |
                    ((uint32_t)block[j*4+2]<<16) | ((uint32_t)block[j*4+3]<<24);
        uint32_t A=a0,B=b0,C=c0,D=d0;
        for (uint32_t i = 0; i < 64; i++) {
            uint32_t F, g;
            if (i < 16)      { F = (B&C)|((~B)&D); g = i; }
            else if (i < 32) { F = (D&B)|((~D)&C); g = (5*i+1)%16; }
            else if (i < 48) { F = B^C^D;           g = (3*i+5)%16; }
            else              { F = C^(B|(~D));      g = (7*i)%16; }
            F += A + K[i] + M[g];
            A = D; D = C; C = B;
            B = B + ((F << S[i]) | (F >> (32 - S[i])));
        }
        a0+=A; b0+=B; c0+=C; d0+=D;
    }

    std::string hexdigest() {
        // Padding
        uint64_t totalBits = totalLen * 8;
        uint8_t pad = (uint8_t)0x80;
        update(&pad, 1);
        pad = 0;
        while (bufLen != 56) update(&pad, 1);
        uint8_t lenBytes[8];
        for (int i = 0; i < 8; i++) lenBytes[i] = (uint8_t)(totalBits >> (8*i));
        update(lenBytes, 8);
        // Digest
        uint32_t digest[4] = {a0, b0, c0, d0};
        char hex[33];
        for (int i = 0; i < 4; i++)
            sprintf(hex + i*8, "%02x%02x%02x%02x",
                    digest[i]&0xff, (digest[i]>>8)&0xff,
                    (digest[i]>>16)&0xff, (digest[i]>>24)&0xff);
        hex[32] = 0;
        return std::string(hex);
    }
};

const uint32_t MD5::S[64] = {
    7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
    5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
    4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
    6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
};
const uint32_t MD5::K[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};

// Skip one CBOR value starting at data[pos]. Returns new position past the value.
// Handles: unsigned int, negative int, byte string, text string, array, map,
// tagged values, and simple values/floats (major types 0-7).
static size_t skipCbor(const uint8_t *data, size_t len, size_t pos) {
    if (pos >= len) return len;
    uint8_t initial = data[pos];
    uint8_t major = initial >> 5;
    uint8_t info  = initial & 0x1f;
    pos++;

    // Read the "argument" (length or value)
    uint64_t arg = 0;
    if (info < 24) {
        arg = info;
    } else if (info == 24) {
        if (pos >= len) return len;
        arg = data[pos++];
    } else if (info == 25) {
        if (pos + 2 > len) return len;
        arg = ((uint64_t)data[pos]<<8) | data[pos+1]; pos += 2;
    } else if (info == 26) {
        if (pos + 4 > len) return len;
        arg = ((uint64_t)data[pos]<<24) | ((uint64_t)data[pos+1]<<16) |
              ((uint64_t)data[pos+2]<<8) | data[pos+3]; pos += 4;
    } else if (info == 27) {
        if (pos + 8 > len) return len;
        for (int i = 0; i < 8; i++) arg = (arg << 8) | data[pos+i];
        pos += 8;
    } else if (info >= 28) {
        // 28-30 reserved; 31 = break/indefinite — not expected at top level
        return pos; // skip just the initial byte
    }

    switch (major) {
        case 0: // unsigned int — already consumed
        case 1: // negative int — already consumed
            return pos;
        case 2: // byte string
        case 3: // text string
            return pos + (size_t)arg;
        case 4: // array of arg items
            for (uint64_t i = 0; i < arg; i++) pos = skipCbor(data, len, pos);
            return pos;
        case 5: // map of arg key-value pairs
            for (uint64_t i = 0; i < arg; i++) {
                pos = skipCbor(data, len, pos); // key
                pos = skipCbor(data, len, pos); // value
            }
            return pos;
        case 6: // tagged value — skip the tag's content
            return skipCbor(data, len, pos);
        case 7: // simple/float — already consumed (float16=2, float32=4, float64=8 extra bytes already handled by arg parsing)
            return pos;
        default:
            return pos;
    }
}

// Parse a CBOR text string at data[pos]. Returns the string and advances pos.
static std::string readCborTextString(const uint8_t *data, size_t len, size_t &pos) {
    if (pos >= len) return "";
    uint8_t info = data[pos] & 0x1f;
    uint8_t major = data[pos] >> 5;
    pos++;
    if (major != 3) return ""; // not a text string
    uint64_t slen = 0;
    if (info < 24) {
        slen = info;
    } else if (info == 24) {
        slen = data[pos++];
    } else if (info == 25) {
        slen = ((uint64_t)data[pos]<<8) | data[pos+1]; pos += 2;
    } else if (info == 26) {
        slen = ((uint64_t)data[pos]<<24) | ((uint64_t)data[pos+1]<<16) |
               ((uint64_t)data[pos+2]<<8) | data[pos+3]; pos += 4;
    } else if (info == 27) {
        for (int i = 0; i < 8; i++) slen = (slen << 8) | data[pos+i];
        pos += 8;
    }
    std::string result((const char *)data + pos, (size_t)slen);
    pos += (size_t)slen;
    return result;
}

// Encode a CBOR text string header + content into output
static void writeCborTextString(std::vector<uint8_t> &out, const std::string &s) {
    size_t n = s.size();
    if (n < 24) {
        out.push_back(0x60 | (uint8_t)n);
    } else if (n < 256) {
        out.push_back(0x78); out.push_back((uint8_t)n);
    } else if (n < 65536) {
        out.push_back(0x79);
        out.push_back((uint8_t)(n >> 8)); out.push_back((uint8_t)n);
    } else {
        out.push_back(0x7a);
        out.push_back((uint8_t)(n >> 24)); out.push_back((uint8_t)(n >> 16));
        out.push_back((uint8_t)(n >> 8));  out.push_back((uint8_t)n);
    }
    out.insert(out.end(), s.begin(), s.end());
}

// Encode a CBOR map header (number of pairs) into output
static void writeCborMapHeader(std::vector<uint8_t> &out, size_t count) {
    if (count < 24) {
        out.push_back(0xa0 | (uint8_t)count);
    } else if (count < 256) {
        out.push_back(0xb8); out.push_back((uint8_t)count);
    } else if (count < 65536) {
        out.push_back(0xb9);
        out.push_back((uint8_t)(count >> 8)); out.push_back((uint8_t)count);
    } else {
        out.push_back(0xba);
        out.push_back((uint8_t)(count >> 24)); out.push_back((uint8_t)(count >> 16));
        out.push_back((uint8_t)(count >> 8));  out.push_back((uint8_t)count);
    }
}

// A span of raw bytes within a CBOR buffer (offset + length).
struct CborValueSpan {
    size_t offset;
    size_t length;
};

// Parse a top-level CBOR map into a vector of (key, value_span) pairs.
// Keys are CBOR text strings. Values are stored as raw byte spans (NOT decoded).
// Preserves insertion order. Returns false on parse error.
static bool parseCborMapEntries(const uint8_t *data, size_t len,
                                std::vector<std::pair<std::string, CborValueSpan>> &entries) {
    if (len == 0) return false;
    uint8_t initial = data[0];
    if ((initial >> 5) != 5) return false; // not a map
    uint8_t info = initial & 0x1f;
    size_t pos = 1;
    uint64_t count = 0;
    if (info < 24) {
        count = info;
    } else if (info == 24) {
        count = data[pos++];
    } else if (info == 25) {
        count = ((uint64_t)data[pos]<<8) | data[pos+1]; pos += 2;
    } else if (info == 26) {
        count = ((uint64_t)data[pos]<<24) | ((uint64_t)data[pos+1]<<16) |
                ((uint64_t)data[pos+2]<<8) | data[pos+3]; pos += 4;
    } else if (info == 27) {
        for (int i = 0; i < 8; i++) count = (count << 8) | data[pos+i];
        pos += 8;
    } else {
        return false;
    }

    entries.reserve((size_t)count);
    for (uint64_t i = 0; i < count; i++) {
        std::string key = readCborTextString(data, len, pos);
        size_t valStart = pos;
        pos = skipCbor(data, len, pos);
        if (pos > len) return false;
        entries.push_back({key, {valStart, pos - valStart}});
    }
    return true;
}

// Keys whose values should be kept from getState (metadata), not replaced from preset
static bool isMetadataKey(const std::string &key) {
    return key == "component" || key == "product" || key == "productVersion" ||
           key == "url" || key == "vendor" || key == "version";
}

} // anonymous namespace

// loadPresetFromFile: read a preset file and feed it to IComponent::setState
// ====================================================================
bool VST3Instrument::loadPresetFromFile(const std::string &filePath, int headerSkipBytes) {
    if (!component_) return false;

    // Log preset name early so we know which file is being loaded if a crash
    // occurs inside Wine/Serum during setState.
    {
        size_t sl = filePath.rfind('/');
        std::string shortName = (sl != std::string::npos) ? filePath.substr(sl+1) : filePath;
    }

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
        } else {
            isZlib = false;
            decompressed.clear();
        }
    }

    // XferJson format handler (Serum 2 .SerumPreset files)
    // ================================================
    // .SerumPreset files are XferJson blobs with different CBOR structure
    // than getState() output (175 vs 162 keys — presets include wavetables,
    // GUI state, extra module data).  setState() only accepts getState's
    // structure.
    //
    // Solution: Binary-safe CBOR merge.
    //   1. Decompress both zstd → CBOR blobs
    //   2. Parse top-level maps into key → raw_value_span (NO value decode)
    //   3. Build merged map using getState's key set: for each key, copy
    //      the preset's raw value bytes if available, else keep getState's.
    //      Metadata keys (component, version, etc.) always stay from getState.
    //   4. Only map headers and key strings are re-encoded (trivial, no floats).
    //   5. Recompress → compute MD5 → assemble XferJson → setState.
    bool isXferJson = (data.size() > 17 &&
                       memcmp(&data[0], "XferJson\0", 9) == 0);
    if (isXferJson) {
      try {
        if (componentDead_) {
            Trace::Error("VST3: cannot load preset — plugin is dead (previous setState timed out)");
            return false;
        }
        IComponent *comp = (IComponent *)component_;
        IAudioProcessor *proc = audioProcessor_ ? (IAudioProcessor *)audioProcessor_ : nullptr;

        // ---- Parse preset XferJson header ----
        uint64_t prJsonLen = 0;
        for (int i = 0; i < 8; i++) prJsonLen |= ((uint64_t)data[9+i]) << (8*i);
        size_t prBinaryOff = 17 + (size_t)prJsonLen;
        if (prBinaryOff + 8 >= data.size()) {
            Trace::Error("VST3: preset XferJson too short");
            return false;
        }
        const uint8_t *prBinSection = &data[prBinaryOff];
        size_t prBinSectionLen = data.size() - prBinaryOff;
        uint32_t prDecompSz = prBinSection[0] | (prBinSection[1]<<8) |
                               (prBinSection[2]<<16) | (prBinSection[3]<<24);
        // uint32_t prBinVer = prBinSection[4]|(prBinSection[5]<<8)|(prBinSection[6]<<16)|(prBinSection[7]<<24);
        const uint8_t *prZstd = prBinSection + 8;
        size_t prZstdLen = prBinSectionLen - 8;

        // ---- Lock + deactivate ----
        componentMutex_.Lock();
        if (proc && isProcessing_) {
            proc->setProcessing(false);
            isProcessing_ = false;
        }
        if (isActive_) {
            comp->setActive(false);
            isActive_ = false;
        }
        struct ComponentGuard {
            IComponent *c; IAudioProcessor *p;
            bool &active; bool &processing;
            bool &dead;
            SysMutex &mtx;
            ComponentGuard(IComponent *c_, IAudioProcessor *p_,
                           bool &a, bool &pr, bool &d, SysMutex &m)
                : c(c_), p(p_), active(a), processing(pr), dead(d), mtx(m) {}
            ~ComponentGuard() {
                if (!dead) {
                    c->setActive(true);  active = true;
                    if (p) { p->setProcessing(true); processing = true; }
                }
                mtx.Unlock();
            }
        } guard(comp, proc, isActive_, isProcessing_, componentDead_, componentMutex_);

        // ---- Get current state ----
        SimpleMemoryStream gsStream;
        tresult gsRes = comp->getState(&gsStream);
        if (gsRes != kResultOk || gsStream.size() < 17) {
            Trace::Error("VST3: getState failed for XferJson merge (res=%d)", (int)gsRes);
            return false;
        }
        const uint8_t *gsRaw = (const uint8_t *)gsStream.data();
        size_t gsRawLen = gsStream.size();
        if (memcmp(gsRaw, "XferJson\0", 9) != 0) {
            Trace::Error("VST3: getState is not XferJson — cannot merge");
            return false;
        }
        uint64_t gsJsonLen = 0;
        for (int i = 0; i < 8; i++) gsJsonLen |= ((uint64_t)gsRaw[9+i]) << (8*i);
        size_t gsBinaryOff = 17 + (size_t)gsJsonLen;
        if (gsBinaryOff + 8 >= gsRawLen) {
            Trace::Error("VST3: getState XferJson too short");
            return false;
        }
        const uint8_t *gsBinSection = gsRaw + gsBinaryOff;
        size_t gsBinSectionLen = gsRawLen - gsBinaryOff;
        uint32_t gsDecompSz = gsBinSection[0] | (gsBinSection[1]<<8) |
                               (gsBinSection[2]<<16) | (gsBinSection[3]<<24);
        uint32_t gsBinVer = gsBinSection[4]|(gsBinSection[5]<<8)|
                             (gsBinSection[6]<<16)|(gsBinSection[7]<<24);
        const uint8_t *gsZstd = gsBinSection + 8;
        size_t gsZstdLen = gsBinSectionLen - 8;

        std::string gsJson((const char*)gsRaw + 17, (size_t)gsJsonLen);

        // ---- Decompress both CBOR blobs ----
        std::vector<uint8_t> gsCbor(gsDecompSz);
        size_t gsActual = ZSTD_decompress(gsCbor.data(), gsDecompSz, gsZstd, gsZstdLen);
        if (ZSTD_isError(gsActual)) {
            Trace::Error("VST3: zstd decompress getState failed: %s", ZSTD_getErrorName(gsActual));
            return false;
        }
        gsCbor.resize(gsActual);

        std::vector<uint8_t> prCbor(prDecompSz);
        size_t prActual = ZSTD_decompress(prCbor.data(), prDecompSz, prZstd, prZstdLen);
        if (ZSTD_isError(prActual)) {
            Trace::Error("VST3: zstd decompress preset failed: %s", ZSTD_getErrorName(prActual));
            return false;
        }
        prCbor.resize(prActual);

        // ---- Parse both CBOR maps ----
        std::vector<std::pair<std::string, CborValueSpan>> gsEntries, prEntries;
        if (!parseCborMapEntries(gsCbor.data(), gsCbor.size(), gsEntries)) {
            Trace::Error("VST3: failed to parse getState CBOR map");
            return false;
        }
        if (!parseCborMapEntries(prCbor.data(), prCbor.size(), prEntries)) {
            Trace::Error("VST3: failed to parse preset CBOR map");
            return false;
        }

        // Build lookup from preset keys → raw value spans
        std::map<std::string, CborValueSpan> prLookup;
        for (auto &e : prEntries) prLookup[e.first] = e.second;

        // ---- Build merged CBOR map ----
        // Use getState's key set (162 keys). For each key:
        //   - metadata keys → keep getState's raw value bytes
        //   - other keys    → replace with preset's raw value bytes if available
        std::vector<uint8_t> merged;
        merged.reserve(prCbor.size() + 1024); // preset values are larger
        writeCborMapHeader(merged, gsEntries.size());

        size_t replaced = 0, kept = 0;
        for (auto &entry : gsEntries) {
            const std::string &key = entry.first;
            writeCborTextString(merged, key);

            if (isMetadataKey(key)) {
                // Keep getState's value
                merged.insert(merged.end(),
                    gsCbor.data() + entry.second.offset,
                    gsCbor.data() + entry.second.offset + entry.second.length);
                kept++;
            } else {
                auto it = prLookup.find(key);
                if (it != prLookup.end()) {
                    // Replace with preset's raw value bytes
                    merged.insert(merged.end(),
                        prCbor.data() + it->second.offset,
                        prCbor.data() + it->second.offset + it->second.length);
                    replaced++;
                } else {
                    // Key not in preset — keep getState's value
                    merged.insert(merged.end(),
                        gsCbor.data() + entry.second.offset,
                        gsCbor.data() + entry.second.offset + entry.second.length);
                    kept++;
                }
            }
        }

        // ---- Recompress with zstd ----
        size_t zstdBound = ZSTD_compressBound(merged.size());
        std::vector<uint8_t> compressed(zstdBound);
        size_t compressedLen = ZSTD_compress(compressed.data(), zstdBound,
                                              merged.data(), merged.size(), 3);
        if (ZSTD_isError(compressedLen)) {
            Trace::Error("VST3: zstd compress merged CBOR failed: %s", ZSTD_getErrorName(compressedLen));
            return false;
        }
        compressed.resize(compressedLen);

        // ---- Compute MD5 of compressed data ----
        MD5 md5;
        md5.update(compressed.data(), compressed.size());
        std::string newHash = md5.hexdigest();

        // ---- Update hash in getState JSON ----
        {
            std::string needle = "\"hash\":\"";
            size_t p = gsJson.find(needle);
            if (p != std::string::npos) {
                size_t s = p + needle.size();
                size_t e = gsJson.find('"', s);
                if (e != std::string::npos)
                    gsJson = gsJson.substr(0, s) + newHash + gsJson.substr(e);
            }
        }

        // ---- Assemble XferJson blob ----
        // Header: "XferJson\0" (9) + JSON_len (8 LE) + JSON + decompSz (4 LE) + binVer (4 LE) + zstd
        uint32_t mergedDecompSz = (uint32_t)merged.size();
        std::vector<uint8_t> output;
        output.reserve(9 + 8 + gsJson.size() + 8 + compressed.size());
        // Magic
        output.insert(output.end(), gsRaw, gsRaw + 9);
        // JSON length (8 LE)
        uint64_t newJsonLen = (uint64_t)gsJson.size();
        for (int i = 0; i < 8; i++)
            output.push_back((uint8_t)(newJsonLen >> (8*i)));
        // JSON body
        output.insert(output.end(), gsJson.begin(), gsJson.end());
        // Binary sub-header: decompressed size (4 LE) + binary version (4 LE)
        for (int i = 0; i < 4; i++) output.push_back((uint8_t)(mergedDecompSz >> (8*i)));
        for (int i = 0; i < 4; i++) output.push_back((uint8_t)(gsBinVer >> (8*i)));
        // Zstd-compressed merged CBOR
        output.insert(output.end(), compressed.begin(), compressed.end());

        // ---- setState with timeout ----
        {
            auto promisePtr = std::make_shared<std::promise<tresult>>();
            auto future = promisePtr->get_future();
            auto stateStreamPtr = std::make_shared<SimpleMemoryStream>();
            stateStreamPtr->setData(&output[0], output.size());
            std::thread t([comp, stateStreamPtr, promisePtr]() {
                try {
                    tresult r = comp->setState(stateStreamPtr.get());
                    promisePtr->set_value(r);
                } catch (const std::exception &ex) {
                    Trace::Error("VST3: setState thread exception: %s", ex.what());
                    try { promisePtr->set_value(kResultFalse); } catch (...) {}
                } catch (...) {
                    Trace::Error("VST3: setState thread unknown exception");
                    try { promisePtr->set_value(kResultFalse); } catch (...) {}
                }
            });
            auto status = future.wait_for(std::chrono::seconds(15));
            if (status == std::future_status::timeout) {
                t.detach();
                componentDead_ = true;
                g_vst3ComponentDead = 1;
                Trace::Error("VST3: setState timed out after 15s — plugin is now DEAD");
                return false;
            }
            t.join();
            tresult setRes = future.get();
            if (setRes != kResultOk) {
                Trace::Error("VST3: setState failed with merged XferJson (res=%d)", (int)setRes);
                return false;
            }
        }

        // ---- Sync controller ----
        if (editController_ && !componentIsController_) {
            IEditController *ctrl = (IEditController *)editController_;
            SimpleMemoryStream syncStream;
            syncStream.setData(&output[0], output.size());
            ctrl->setComponentState(&syncStream);
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
        return true;
      } catch (const std::bad_alloc &) {
        Trace::Error("VST3: Out of memory loading XferJson preset: %s", filePath.c_str());
        return false;
      } catch (const std::exception &ex) {
        Trace::Error("VST3: Exception during preset load: %s", ex.what());
        return false;
      } catch (...) {
        Trace::Error("VST3: Unknown exception during preset load");
        return false;
      }
    }

    // For JUCE-based plugins whose preset files are raw XML, we need to
    // wrap the data in JUCE's copyXmlToBinary binary format before calling
    // setState.  JUCE's setStateInformation calls getXmlFromBinary() which
    // expects: [4-byte magic 0x21324356][4-byte string length LE][XML][0x00].
    // Without this wrapper, getXmlFromBinary returns nullptr and the preset
    // is silently ignored.
    {
        static const char *jucePresetExts[] = { ".pivot", ".noisemakerpreset", nullptr };
        bool needsJuceWrap = false;
        for (int i = 0; jucePresetExts[i]; i++) {
            size_t el = strlen(jucePresetExts[i]);
            if (filePath.size() > el &&
                filePath.substr(filePath.size() - el) == jucePresetExts[i]) {
                needsJuceWrap = true;
                break;
            }
        }
        if (needsJuceWrap && !data.empty() && data[0] == '<') {
            uint32_t magic = 0x21324356;  // JUCE magicXmlNumber
            uint32_t strLen = (uint32_t)data.size();
            std::vector<uint8_t> wrapped(8 + data.size() + 1);
            memcpy(&wrapped[0], &magic, 4);
            memcpy(&wrapped[4], &strLen, 4);
            memcpy(&wrapped[8], &data[0], data.size());
            wrapped[8 + data.size()] = 0;
            data = std::move(wrapped);
        }
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
            loaded = true;
        } else {
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

    // Activate bus 0 for input/output, explicitly deactivate all others.
    // Some plugins (e.g. Serum 2 via yabridge) crash if unused buses are
    // not explicitly deactivated.
    for (int32 i = 0; i < numOutputBuses; i++) {
        comp->activateBus(kAudio, kOutput, i, i == 0);
    }

    for (int32 i = 0; i < numInputBuses; i++) {
        comp->activateBus(kAudio, kInput, i, i == 0);
    }

    // Build speaker arrangement arrays matching the full bus count.
    // The VST3 spec requires the array sizes to match the plugin's bus
    // count — passing fewer elements causes out-of-bounds reads in some
    // plugins (Serum 2 via yabridge in game mode).
    if (numOutputBuses > 0) {
        std::vector<SpeakerArrangement> outputs(numOutputBuses, 0);
        outputs[0] = stereo;

        if (numInputBuses > 0) {
            std::vector<SpeakerArrangement> inputs(numInputBuses, 0);
            inputs[0] = stereo;
            proc->setBusArrangements(inputs.data(), numInputBuses, outputs.data(), numOutputBuses);
        } else {
            proc->setBusArrangements(nullptr, 0, outputs.data(), numOutputBuses);
        }
    }

    // Activate event (MIDI) buses
    for (int32 i = 0; i < numEventInputBuses; i++) {
        comp->activateBus(kEvent, kInput, i, true);
    }

    // Setup processing — zero-init to avoid garbage in padding bytes
    // that yabridge may serialize across the IPC boundary.
    ProcessSetup setup;
    memset(&setup, 0, sizeof(setup));
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

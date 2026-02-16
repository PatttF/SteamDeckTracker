/**
 * VST3Effect.cpp — headless VST3 effect plugin hosting
 *
 * Loads a .vst3 bundle, creates the processor and controller,
 * and processes audio in-place on interleaved fixed-point stereo buffers.
 * Adapted from VST3Instrument but specialized for audio effects
 * (no MIDI note handling, has wet/dry mix).
 */

#include "VST3Effect.h"
#include "EfxFragmentsPresetData.h"
#include "TALReverb4PresetData.h"
#include "ValhallaDelayPresets.h"
#include "ValhallaPlatePresets.h"
#include "ValhallaRoomPresets.h"
#include "ValhallaSpaceModulatorPresets.h"
#include "ValhallaSupermassivePresets.h"
#include "ValhallaUberModPresets.h"
#include "ValhallaVintageVerbPresets.h"
#include "Application/Model/Config.h"
#include "Foundation/Variables/WatchedVariable.h"
#include "Services/Audio/Audio.h"
#include "System/Console/Trace.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <exception>
#include <fstream>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>

// VST3 SDK headers
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstunits.h"
#include "pluginterfaces/vst/vsttypes.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

// ---- Module entry/exit function typedefs (Linux) ----
extern "C" {
typedef bool (*VST3FXModuleEntryFunc)(void*);
typedef bool (*VST3FXModuleExitFunc)();
typedef IPluginFactory* (*VST3FXGetFactoryFunc)();
}

// ====================================================================
// Host application (reuse from VST3Instrument — extern linkage)
// ====================================================================
// These are defined in VST3Instrument.cpp
extern Steinberg::FUnknown* getVST3HostApp();

// ====================================================================
// Lightweight helper classes (same pattern as VST3Instrument.cpp)
// We re-declare them here as local classes to avoid linkage issues.
// ====================================================================

namespace {

class FXHostApp : public IHostApplication {
public:
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
        if (refCount_ > 1) --refCount_;
        return refCount_;
    }
    tresult PLUGIN_API getName(String128 name) override {
        const char16_t n[] = u"SteamDeckTracker";
        memcpy(name, n, sizeof(n));
        return kResultTrue;
    }
    tresult PLUGIN_API createInstance(TUID, TUID, void** obj) override {
        *obj = nullptr;
        return kNotImplemented;
    }
private:
    int32_t refCount_ = 1;
};

static FXHostApp gFXHostApp;

class FXComponentHandler : public IComponentHandler {
public:
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
    tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID, ParamValue) override { return kResultOk; }
    tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32) override { return kResultOk; }
private:
    int32_t refCount_ = 1;
};

class FXEventList : public IEventList {
public:
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(_iid, IEventList::iid) ||
            FUnknownPrivate::iidEqual(_iid, FUnknown::iid)) {
            *obj = static_cast<IEventList*>(this);
            return kResultTrue;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    int32 PLUGIN_API getEventCount() override { return 0; }
    tresult PLUGIN_API getEvent(int32, Event&) override { return kInvalidArgument; }
    tresult PLUGIN_API addEvent(Event&) override { return kResultOk; }
};

class FXParamValueQueue : public IParamValueQueue {
public:
    FXParamValueQueue() : paramId_(0) {}
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
    tresult PLUGIN_API getPoint(int32 index, int32& off, ParamValue& val) override {
        if (index < 0 || index >= (int32)points_.size()) return kInvalidArgument;
        off = points_[index].first;
        val = points_[index].second;
        return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32 off, ParamValue val, int32& idx) override {
        idx = (int32)points_.size();
        points_.push_back({off, val});
        return kResultOk;
    }
    void clear() { points_.clear(); }
private:
    ParamID paramId_;
    std::vector<std::pair<int32, ParamValue>> points_;
};

class FXParameterChanges : public IParameterChanges {
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
        for (int32 i = 0; i < (int32)queues_.size(); i++) {
            if (queues_[i].getParameterId() == id) {
                index = i;
                return &queues_[i];
            }
        }
        index = (int32)queues_.size();
        queues_.push_back(FXParamValueQueue());
        queues_.back().setParamId(id);
        return &queues_.back();
    }
private:
    std::vector<FXParamValueQueue> queues_;
};

class FXMemoryStream : public IBStream {
public:
    FXMemoryStream() : pos_(0), refCount_(1) {}
    virtual ~FXMemoryStream() {}
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
        if (toRead > 0) memcpy(buffer, &data_[pos_], toRead);
        pos_ += toRead;
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
    void setData(const uint8_t *d, size_t len) { data_.assign(d, d + len); pos_ = 0; }
private:
    std::vector<uint8_t> data_;
    size_t pos_;
    int32_t refCount_;
};

// Helpers (same as VST3Instrument.cpp)
static std::string getArchString() {
    struct utsname uts;
    if (uname(&uts) == 0) return std::string(uts.machine) + "-linux";
    return "x86_64-linux";
}

static std::string resolveSoPath(const std::string& bundlePath) {
    std::string arch = getArchString();
    std::string stem = bundlePath;
    size_t lastSlash = stem.rfind('/');
    if (lastSlash != std::string::npos) stem = stem.substr(lastSlash + 1);
    size_t extPos = stem.rfind(".vst3");
    if (extPos != std::string::npos) stem = stem.substr(0, extPos);
    std::string soPath = bundlePath + "/Contents/" + arch + "/" + stem + ".so";
    struct stat st;
    if (stat(soPath.c_str(), &st) == 0) return soPath;
    return "";
}

static void findBundles(const std::string& dir, std::vector<std::string>& out) {
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
            if (name.size() > 5 && name.substr(name.size() - 4) == "vst3" &&
                name[name.size()-5] == '.') {
                out.push_back(full);
            } else {
                findBundles(full, out);
            }
        }
    }
    closedir(d);
}

static std::string tuidHex(const uint8_t* tuid) {
    char buf[33];
    for (int i = 0; i < 16; i++) sprintf(buf + i * 2, "%02X", tuid[i]);
    buf[32] = '\0';
    return std::string(buf);
}

static void hexTuid(const std::string& hex, uint8_t out[16]) {
    memset(out, 0, 16);
    for (int i = 0; i < 16 && i * 2 + 1 < (int)hex.size(); i++) {
        unsigned int val;
        sscanf(hex.c_str() + i * 2, "%02X", &val);
        out[i] = (uint8_t)val;
    }
}

static void str128ToChar(const TChar* src, char* dst, int maxLen) {
    int i = 0;
    while (i < maxLen - 1 && src[i]) {
        dst[i] = (char)(src[i] & 0x7F);
        i++;
    }
    dst[i] = '\0';
}

} // anonymous namespace

// ====================================================================
// VST3Effect implementation
// ====================================================================

VST3Effect::VST3Effect() {
    strcpy(name_, "VST3 Effect");
    memset(pluginPath_, 0, sizeof(pluginPath_));
    memset(pluginClassId_, 0, sizeof(pluginClassId_));

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

    isActive_ = false;
    isProcessing_ = false;
    forcedOutputChannels_ = 2;

    // Add volume and wet/dry variables
    Variable *vol = new Variable("volume", VST3FX_VOLUME, 0xFF);
    Insert(vol);
    Variable *wet = new Variable("wet", VST3FX_WETDRY, 0xFF);
    Insert(wet);

    currentBank_ = 0;
    currentPreset_ = 0;
    programChangeParamIdx_ = -1;
    usingFilePresets_ = false;
    usingEmbeddedPresets_ = false;
}

VST3Effect::~VST3Effect() {
    Purge();
}

bool VST3Effect::Init() {
    return true;
}

const char *VST3Effect::GetName() const {
    if (IsEmpty()) return "-- no effect --";
    return name_;
}

void VST3Effect::SetForcedOutputChannels(int count) {
    forcedOutputChannels_ = count;
}

void VST3Effect::Purge() {
    // Remove observers from parameter variables
    for (size_t i = 0; i < parameters_.size(); ++i) {
        if (parameters_[i].variable) {
            WatchedVariable *wv = dynamic_cast<WatchedVariable*>(parameters_[i].variable);
            if (wv) wv->RemoveObserver(*this);
        }
    }
    pluginMutex_.Lock();
    cleanupPlugin();
    pluginPath_[0] = '\0';
    memset(pluginClassId_, 0, sizeof(pluginClassId_));
    strcpy(name_, "VST3 Effect");
    programLists_.clear();
    filePresetsByBank_.clear();
    usingFilePresets_ = false;
    currentBank_ = 0;
    currentPreset_ = 0;
    programChangeParamIdx_ = -1;
    pluginMutex_.Unlock();
    parameters_.clear();
    effectParams_.clear();
}

void VST3Effect::SetPlugin(const char *path, const uint8_t classId[16]) {
    Purge();
    strncpy(pluginPath_, path, sizeof(pluginPath_) - 1);
    pluginPath_[sizeof(pluginPath_) - 1] = '\0';
    memcpy(pluginClassId_, classId, 16);
    loadPlugin();
}

void VST3Effect::loadPlugin() {
    if (pluginPath_[0] == '\0') return;

    try {
        loadPluginInner();
    } catch (const std::exception& ex) {
        Trace::Error("VST3FX: Exception loading plugin: %s", ex.what());
        cleanupPlugin();
    } catch (...) {
        Trace::Error("VST3FX: Unknown exception loading plugin %s", pluginPath_);
        cleanupPlugin();
    }
}

void VST3Effect::loadPluginInner() {

    std::string soPath = resolveSoPath(pluginPath_);
    if (soPath.empty()) {
        Trace::Error("VST3FX: Could not resolve .so path for %s", pluginPath_);
        return;
    }

    moduleHandle_ = dlopen(soPath.c_str(), RTLD_LAZY);
    if (!moduleHandle_) {
        Trace::Error("VST3FX: dlopen failed: %s", dlerror());
        return;
    }

    VST3FXModuleEntryFunc moduleEntry = (VST3FXModuleEntryFunc)dlsym(moduleHandle_, "ModuleEntry");
    if (!moduleEntry || !moduleEntry(moduleHandle_)) {
        Trace::Error("VST3FX: ModuleEntry failed");
        dlclose(moduleHandle_);
        moduleHandle_ = nullptr;
        return;
    }

    VST3FXGetFactoryFunc getFactory = (VST3FXGetFactoryFunc)dlsym(moduleHandle_, "GetPluginFactory");
    if (!getFactory) {
        Trace::Error("VST3FX: No GetPluginFactory");
        VST3FXModuleExitFunc moduleExit = (VST3FXModuleExitFunc)dlsym(moduleHandle_, "ModuleExit");
        if (moduleExit) moduleExit();
        dlclose(moduleHandle_);
        moduleHandle_ = nullptr;
        return;
    }

    IPluginFactory* factory = getFactory();
    if (!factory) {
        Trace::Error("VST3FX: GetPluginFactory returned null");
        VST3FXModuleExitFunc moduleExit = (VST3FXModuleExitFunc)dlsym(moduleHandle_, "ModuleExit");
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
                    
                    if (factory2) {
                        PClassInfo2 ci2;
                        if (factory2->getClassInfo2(i, &ci2) == kResultOk) {
                            resolvedSub = ci2.subCategories;
                            
                        }
                    }
                    break;
                }
            }
            if (factory2) factory2->release();

            // Write resolved CID and subcategories back to moduleinfo.json
            if (!resolvedSub.empty()) {
                std::string jsonPath =
                    std::string(pluginPath_) + "/Contents/moduleinfo.json";
                struct stat jsonSt;
                if (stat(jsonPath.c_str(), &jsonSt) == 0) {
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
                            tuidHex(pluginClassId_) + "\",\n";
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

    // Create processor component
    IComponent* component = nullptr;
    tresult res = factory->createInstance(
        (FIDString)pluginClassId_, IComponent::iid, (void**)&component);
    if (res != kResultOk || !component) {
        Trace::Error("VST3FX: Failed to create component");
        cleanupPlugin();
        return;
    }
    component_ = component;

    res = component->initialize(static_cast<FUnknown*>(&gFXHostApp));
    if (res != kResultOk) {
        Trace::Error("VST3FX: Component init failed");
        cleanupPlugin();
        return;
    }

    // Get IAudioProcessor
    IAudioProcessor* audioProc = nullptr;
    res = component->queryInterface(IAudioProcessor::iid, (void**)&audioProc);
    if (res != kResultOk || !audioProc) {
        Trace::Error("VST3FX: No IAudioProcessor");
        cleanupPlugin();
        return;
    }
    audioProcessor_ = audioProc;

    // Get IEditController
    IEditController* controller = nullptr;
    res = component->queryInterface(IEditController::iid, (void**)&controller);
    if (res == kResultOk && controller) {
        editController_ = controller;
        componentIsController_ = true;
    } else {
        TUID controllerCID;
        res = component->getControllerClassId(controllerCID);
        if (res == kResultOk) {
            res = factory->createInstance(controllerCID, IEditController::iid, (void**)&controller);
            if (res == kResultOk && controller) {
                controller->initialize(static_cast<FUnknown*>(&gFXHostApp));
                editController_ = controller;
                componentIsController_ = false;
            }
        }
    }

    // Set component handler
    if (editController_) {
        IEditController* ctrl = (IEditController*)editController_;
        FXComponentHandler* handler = new FXComponentHandler();
        ctrl->setComponentHandler(handler);
        handler->release();

        // Sync state for separate controllers
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

            FXMemoryStream stream;
            if (component->getState(&stream) == kResultOk && stream.size() > 0) {
                stream.resetRead();
                ctrl->setComponentState(&stream);
            }
        }
    }

    // Extract plugin name
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

    discoverParameters();
    discoverPresets();

    // Pre-initialize processing on the main thread so potentially slow
    // IPC (yabridge/Wine) doesn't block the audio render thread.
    {
        int sampleRate = Audio::GetInstance()->GetSampleRate();
        if (sampleRate <= 0) sampleRate = 44100;
        int initBlock = 4096;

        audioBufferL_ = (float*)calloc(initBlock, sizeof(float));
        audioBufferR_ = (float*)calloc(initBlock, sizeof(float));
        audioInputL_ = (float*)calloc(initBlock, sizeof(float));
        audioInputR_ = (float*)calloc(initBlock, sizeof(float));
        bufferSize_ = initBlock;

        setupProcessing(initBlock);
        
    }

    
}

void VST3Effect::discoverParameters() {
    if (!editController_) return;

    IEditController* ctrl = (IEditController*)editController_;
    int32 paramCount = ctrl->getParameterCount();

    parameters_.clear();

    for (int32 i = 0; i < paramCount; i++) {
        ParameterInfo info;
        if (ctrl->getParameterInfo(i, info) != kResultOk) continue;
        if (info.flags & ParameterInfo::kIsHidden) continue;
        if (info.flags & ParameterInfo::kIsReadOnly) continue;

        VST3PluginParameter param;
        char titleBuf[128];
        str128ToChar(info.title, titleBuf, 128);
        param.name = titleBuf;

        char unitsBuf[128];
        str128ToChar(info.units, unitsBuf, 128);
        param.units = unitsBuf;

        param.paramId = info.id;
        param.stepCount = info.stepCount;
        param.defaultValue = ctrl->normalizedParamToPlain(info.id, info.defaultNormalizedValue);
        param.currentValue = info.defaultNormalizedValue;
        param.isReadOnly = false;
        param.isBypass = (info.flags & ParameterInfo::kIsBypass) != 0;
        param.isList = (info.flags & ParameterInfo::kIsList) != 0;
        param.minValue = ctrl->normalizedParamToPlain(info.id, 0.0);
        param.maxValue = ctrl->normalizedParamToPlain(info.id, 1.0);
        param.variable = nullptr;

        // Create a Variable for UI binding (0–127 or stepCount if small)
        int maxVal = (param.stepCount > 0 && param.stepCount <= 255) ? param.stepCount : 127;
        int initVal = (int)(param.currentValue * maxVal);
        if (initVal < 0) initVal = 0;
        if (initVal > maxVal) initVal = maxVal;

        char varName[80];
        snprintf(varName, sizeof(varName), "p%d", (int)parameters_.size());
        WatchedVariable *wv = new WatchedVariable(varName, 0, initVal);
        Insert(wv);
        wv->AddObserver(*this);
        param.variable = wv;

        parameters_.push_back(param);
    }

    // Apply pending variable values from project load
    for (auto &pv : pendingParamValues_) {
        for (size_t i = 0; i < parameters_.size(); i++) {
            if (parameters_[i].variable && pv.first == parameters_[i].variable->GetName()) {
                parameters_[i].variable->SetInt(atoi(pv.second.c_str()));
                // Sync the actual plugin parameter
                int maxVal = (parameters_[i].stepCount > 0 && parameters_[i].stepCount <= 255)
                    ? parameters_[i].stepCount : 127;
                double norm = (double)atoi(pv.second.c_str()) / (double)maxVal;
                if (norm < 0.0) norm = 0.0;
                if (norm > 1.0) norm = 1.0;
                parameters_[i].currentValue = norm;
                break;
            }
        }
    }
    pendingParamValues_.clear();

    buildEffectParams();
}

void VST3Effect::buildEffectParams() {
    effectParams_.clear();
    for (size_t i = 0; i < parameters_.size(); i++) {
        EffectParameter ep;
        ep.name = parameters_[i].name;
        ep.groupName = "";
        ep.minValue = (float)parameters_[i].minValue;
        ep.maxValue = (float)parameters_[i].maxValue;
        ep.currentValue = (float)parameters_[i].currentValue;
        ep.variable = parameters_[i].variable;
        effectParams_.push_back(ep);
    }
}

const EffectParameter* VST3Effect::GetEffectParameter(int index) const {
    if (index >= 0 && index < (int)effectParams_.size()) {
        return &effectParams_[index];
    }
    return nullptr;
}

std::string VST3Effect::GetParameterScalePointLabel(int paramIndex, float value) const {
    if (paramIndex < 0 || paramIndex >= (int)parameters_.size()) return "";
    // VST3: use IEditController::getParamStringByValue for display
    if (editController_) {
        IEditController* ctrl = (IEditController*)editController_;
        double norm = value;
        // Denormalize if needed
        if (parameters_[paramIndex].maxValue != parameters_[paramIndex].minValue) {
            norm = (value - parameters_[paramIndex].minValue) /
                   (parameters_[paramIndex].maxValue - parameters_[paramIndex].minValue);
        }
        String128 displayStr;
        if (ctrl->getParamStringByValue(parameters_[paramIndex].paramId, norm, displayStr) == kResultOk) {
            char buf[128];
            str128ToChar(displayStr, buf, 128);
            if (buf[0] != '\0') return std::string(buf);
        }
    }
    return "";
}

const VST3PluginParameter* VST3Effect::GetVST3Parameter(int index) const {
    if (index >= 0 && index < (int)parameters_.size()) {
        return &parameters_[index];
    }
    return nullptr;
}

// ====================================================================
// File-based preset support for VST3 effects
// ====================================================================

// Reuse the same struct layout as VST3Instrument for effect preset mappings
struct VST3FxPresetMapping {
    const char *pluginNameSubstring;
    const char *directories[6];
    const char *extension;
    int headerSkipBytes;
};

static const VST3FxPresetMapping knownFxPresetMappings[] = {
    // Valhalla DSP effects: .vpreset XML files, JUCE-wrapped before setState.
    // All 9 Valhalla plugins share the same format and directory pattern.
    {
        "Valhalla",
        {
            nullptr  // Directories resolved at runtime from WINEPREFIX
        },
        ".vpreset",
        0
    },
    // Arturia Efx FRAGMENTS: NKS .nksf files containing VST3 state in RIFF/NIKS
    // PCHK chunk. Presets live under ProgramData/Arturia/Efx FRAGMENTS/.
    {
        "Efx FRAGMENTS",
        {
            nullptr  // Directories resolved at runtime from WINEPREFIX
        },
        ".nksf",
        0
    },
    // Sentinel
    { nullptr, {nullptr}, nullptr, 0 }
};

static void findFxPresetFiles(const std::string &dir, const char *extension,
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
            std::string subCat = category.empty() ? name : category + "/" + name;
            findFxPresetFiles(full, extension, subCat, bankMap);
        } else if (S_ISREG(st.st_mode)) {
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

// ====================================================================
// discoverPresetFiles: scan filesystem for native effect preset files
// ====================================================================

void VST3Effect::discoverPresetFiles() {
    const VST3FxPresetMapping *mapping = nullptr;
    for (int i = 0; knownFxPresetMappings[i].pluginNameSubstring != nullptr; i++) {
        if (strstr(name_, knownFxPresetMappings[i].pluginNameSubstring) != nullptr) {
            mapping = &knownFxPresetMappings[i];
            break;
        }
    }
    if (!mapping) return;

    std::string homeDir;
    const char *home = getenv("HOME");
    if (home) homeDir = home;

    std::vector<std::string> scanDirs;
    for (int i = 0; i < 6 && mapping->directories[i] != nullptr; i++) {
        scanDirs.push_back(mapping->directories[i]);
    }

    // Add user-specific directories based on plugin type
    if (!homeDir.empty()) {
        std::string pfxBase;
        const char *wpEnv = getenv("WINEPREFIX");
        if (wpEnv && wpEnv[0]) {
            pfxBase = wpEnv;
        } else {
            pfxBase = homeDir + "/Documents/SDTracker/wineprefix";
        }

        if (strstr(name_, "Valhalla") != nullptr) {
            // Valhalla plugins store presets in Wine prefix under
            // Application Data/Valhalla DSP, LLC/<PluginName>/User Presets
            const char *userNames[] = { "steamuser", nullptr };
            const char *prefixes[] = { "", "/pfx", nullptr };
            const char *hostUser = getenv("USER");
            for (int pi = 0; prefixes[pi] != nullptr; pi++) {
                for (int ui = 0; userNames[ui] != nullptr; ui++) {
                    std::string base = pfxBase + prefixes[pi]
                        + "/drive_c/users/" + userNames[ui]
                        + "/Application Data/Valhalla DSP, LLC/"
                        + std::string(name_) + "/User Presets";
                    scanDirs.push_back(base);
                }
                if (hostUser && hostUser[0] &&
                    strcmp(hostUser, "steamuser") != 0) {
                    std::string base = pfxBase + prefixes[pi]
                        + "/drive_c/users/" + hostUser
                        + "/Application Data/Valhalla DSP, LLC/"
                        + std::string(name_) + "/User Presets";
                    scanDirs.push_back(base);
                }
            }
        }

        if (strstr(name_, "Efx FRAGMENTS") != nullptr ||
            strstr(name_, "FRAGMENTS") != nullptr) {
            // Arturia Efx FRAGMENTS: NKS presets under ProgramData
            const char *prefixes[] = { "", "/pfx", nullptr };
            for (int pi = 0; prefixes[pi] != nullptr; pi++) {
                std::string base = pfxBase + prefixes[pi]
                    + "/drive_c/ProgramData/Arturia/Efx FRAGMENTS"
                      "/Third Party/Native Instruments/presets";
                scanDirs.push_back(base);
            }
        }
    }

    std::map<std::string, std::vector<VST3FilePreset>> bankMap;
    for (size_t i = 0; i < scanDirs.size(); i++) {
        findFxPresetFiles(scanDirs[i], mapping->extension, "", bankMap);
    }

    if (bankMap.empty()) return;

    programLists_.clear();
    filePresetsByBank_.clear();
    usingFilePresets_ = true;

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
// loadPresetFromFile: load a preset file and feed it to setState
// ====================================================================

bool VST3Effect::loadPresetFromFile(const std::string &filePath, int headerSkipBytes) {
    if (!component_) return false;

    // Use POSIX I/O to avoid the project's fopen macro redirect
    int fd = ::open(filePath.c_str(), O_RDONLY);
    if (fd < 0) {
        Trace::Error("VST3FX: Cannot open preset file: %s", filePath.c_str());
        return false;
    }

    off_t fileSize = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);

    if (fileSize <= headerSkipBytes) {
        Trace::Error("VST3FX: Preset file too small (%ld bytes): %s", (long)fileSize, filePath.c_str());
        ::close(fd);
        return false;
    }

    // Read entire file for NKSF parsing or headerSkipBytes handling
    std::vector<uint8_t> fileData(fileSize);
    ssize_t bytesRead = ::read(fd, &fileData[0], fileSize);
    ::close(fd);

    if (bytesRead != fileSize) {
        Trace::Error("VST3FX: Short read on preset file: %s", filePath.c_str());
        return false;
    }

    // ---------------------------------------------------------------
    // NKS .nksf support: RIFF/NIKS container with PCHK chunk holding
    // the VST3 component + controller state.
    // Format: RIFF(NIKS) { NISI, NICA, PLID, PCHK }
    // PCHK data: [4-byte NKS version][16-byte header][comp state][ctrl state]
    //   header = [4-byte compSize LE][4-byte pad][4-byte ctrlSize LE][4-byte pad]
    // ---------------------------------------------------------------
    bool isNksf = false;
    {
        size_t el = 5; // strlen(".nksf")
        if (filePath.size() > el &&
            filePath.substr(filePath.size() - el) == ".nksf") {
            isNksf = true;
        }
    }

    if (isNksf && fileData.size() >= 12 &&
        memcmp(&fileData[0], "RIFF", 4) == 0 &&
        memcmp(&fileData[8], "NIKS", 4) == 0) {

        // Find PCHK chunk in RIFF
        const uint8_t *base = &fileData[0];
        size_t total = fileData.size();
        size_t pos = 12; // skip RIFF header + form type
        const uint8_t *pchkData = nullptr;
        uint32_t pchkSize = 0;

        while (pos + 8 <= total) {
            uint32_t chunkSize;
            memcpy(&chunkSize, base + pos + 4, 4); // LE size
            if (memcmp(base + pos, "PCHK", 4) == 0) {
                pchkData = base + pos + 8;
                pchkSize = chunkSize;
                break;
            }
            pos += 8 + chunkSize;
            if (pos & 1) pos++; // RIFF chunks are word-aligned
        }

        if (!pchkData || pchkSize < 24) {
            Trace::Error("VST3FX: NKSF missing PCHK chunk: %s", filePath.c_str());
            return false;
        }

        // NKS PCHK layout (after 8-byte RIFF chunk header):
        //   [4] NKS version (typically 1)
        //   [4] component state size (LE)
        //   [4] padding (0)
        //   [4] controller state size (LE)
        //   [4] padding (0)
        //   [compSize bytes] component state (Boost archive for Arturia)
        //   [ctrlSize bytes] controller state (identical copy for Arturia)
        // We feed the component state to both setState and setComponentState.
        uint32_t compSize;
        memcpy(&compSize, pchkData + 4, 4);  // after NKS version

        const uint8_t *compData = pchkData + 4 + 16;  // skip version + 16-byte header
        uint32_t available = pchkSize - 4 - 16;
        if (compSize > available) compSize = available;

        IComponent *comp = (IComponent *)component_;
        FXMemoryStream compStream;
        compStream.setData(compData, compSize);
        tresult res = comp->setState(&compStream);
        if (res != kResultOk) {
            Trace::Error("VST3FX: NKSF setState failed: %s (result=%d)",
                filePath.c_str(), (int)res);
            return false;
        }

        // Sync controller with the same component state
        if (editController_) {
            IEditController *ctrl = (IEditController *)editController_;
            if (!componentIsController_) {
                compStream.resetRead();
                ctrl->setComponentState(&compStream);
            }

            // Sync parameter values back into our Variables
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

    // ---------------------------------------------------------------
    // Non-NKSF: use headerSkipBytes and existing logic
    // ---------------------------------------------------------------
    std::vector<uint8_t> data;
    if (headerSkipBytes > 0 && (size_t)headerSkipBytes < fileData.size()) {
        data.assign(fileData.begin() + headerSkipBytes, fileData.end());
    } else {
        data = std::move(fileData);
    }

    // Zlib decompression attempt
    std::vector<uint8_t> decompressed;
    bool isZlib = (data.size() >= 2 && data[0] == 0x78);
    if (isZlib) {
        uLongf destLen = (uLongf)data.size() * 10;
        decompressed.resize(destLen);
        int zret = uncompress(&decompressed[0], &destLen, &data[0], (uLong)data.size());
        if (zret == Z_BUF_ERROR) {
            destLen = (uLongf)data.size() * 50;
            decompressed.resize(destLen);
            zret = uncompress(&decompressed[0], &destLen, &data[0], (uLong)data.size());
        }
        if (zret == Z_OK) {
            decompressed.resize(destLen);
        } else {
            isZlib = false;
            decompressed.clear();
        }
    }

    // For JUCE-based plugins (Valhalla DSP, etc.) whose presets are raw XML,
    // wrap in JUCE's copyXmlToBinary format:
    //   [4-byte magic 0x21324356][4-byte string length LE][XML][0x00]
    {
        static const char *jucePresetExts[] = { ".vpreset", nullptr };
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

    // Feed data to IComponent::setState
    IComponent *comp = (IComponent *)component_;
    FXMemoryStream stream;
    bool loaded = false;

    // Try zlib-decompressed data first
    if (!loaded && isZlib && !decompressed.empty()) {
        stream.setData(&decompressed[0], decompressed.size());
        tresult res = comp->setState(&stream);
        if (res == kResultOk) loaded = true;
    }

    // Fall back to raw/wrapped data
    if (!loaded) {
        stream.setData(&data[0], data.size());
        tresult res = comp->setState(&stream);
        if (res != kResultOk) {
            Trace::Error("VST3FX: setState failed for preset file: %s (result=%d)",
                filePath.c_str(), (int)res);
            return false;
        }
    }

    // Sync controller
    if (editController_ && !componentIsController_) {
        IEditController *ctrl = (IEditController *)editController_;
        stream.resetRead();
        ctrl->setComponentState(&stream);
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
}

// ====================================================================
// loadPresetFromXmlData: load a preset from embedded XML string data.
// Wraps in JUCE binary format and feeds to setState, same as file path.
// ====================================================================

bool VST3Effect::loadPresetFromXmlData(const char *xmlData) {
    if (!component_ || !xmlData) return false;

    size_t xmlLen = strlen(xmlData);
    if (xmlLen == 0) return false;

    IComponent *comp = (IComponent *)component_;

    fprintf(stderr, "[VST3FX] loadPresetFromXmlData: xmlLen=%d\n", (int)xmlLen);

    // JUCE copyXmlToBinary format:
    //   [4-byte magic 0x21324356][4-byte string length LE][XML][0x00]
    uint32_t magic = 0x21324356;
    uint32_t strLen = (uint32_t)xmlLen;
    std::vector<uint8_t> data(8 + xmlLen + 1);
    memcpy(&data[0], &magic, 4);
    memcpy(&data[4], &strLen, 4);
    memcpy(&data[8], xmlData, xmlLen);
    data[8 + xmlLen] = 0;

    FXMemoryStream stream;
    stream.setData(&data[0], data.size());
    fprintf(stderr, "[VST3FX] calling setState with %d bytes...\n", (int)data.size());
    tresult res = comp->setState(&stream);
    fprintf(stderr, "[VST3FX] setState result=%d\n", (int)res);
    if (res != kResultOk) {
        fprintf(stderr, "[VST3FX] embedded setState FAILED (result=%d)\n", (int)res);
        return false;
    }

    // Sync controller
    if (editController_ && !componentIsController_) {
        IEditController *ctrl = (IEditController *)editController_;
        stream.resetRead();
        ctrl->setComponentState(&stream);
        fprintf(stderr, "[VST3FX] setComponentState done\n");
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
        // Log first few param values for debugging
        int toLog = parameters_.size() < 8 ? (int)parameters_.size() : 8;
        for (int i = 0; i < toLog; i++) {
            fprintf(stderr, "[VST3FX]   param[%d] '%s' = %.6f\n",
                i, parameters_[i].name, parameters_[i].currentValue);
        }
    }

    return true;
}

// ====================================================================
// loadPresetFromCompressedData: decompress zlib data and feed to setState
// Used for Arturia Efx FRAGMENTS whose PCHK states are binary (not XML).
// ====================================================================
bool VST3Effect::loadPresetFromCompressedData(const uint8_t *compressed,
                                               uint32_t compSize,
                                               uint32_t origSize) {
    if (!component_ || !compressed || compSize == 0 || origSize == 0) return false;

    // Decompress
    std::vector<uint8_t> data(origSize);
    uLongf destLen = (uLongf)origSize;
    int zret = uncompress(&data[0], &destLen, compressed, (uLong)compSize);
    if (zret != Z_OK) {
        Trace::Error("VST3FX: zlib decompress failed (ret=%d)", zret);
        return false;
    }
    data.resize(destLen);

    // Feed directly to setState (Arturia uses Boost serialization, not JUCE)
    IComponent *comp = (IComponent *)component_;
    FXMemoryStream stream;
    stream.setData(&data[0], data.size());
    tresult res = comp->setState(&stream);
    if (res != kResultOk) {
        Trace::Error("VST3FX: compressed setState FAILED (result=%d)", (int)res);
        return false;
    }

    // Sync controller
    if (editController_ && !componentIsController_) {
        IEditController *ctrl = (IEditController *)editController_;
        stream.resetRead();
        ctrl->setComponentState(&stream);
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
}

// ====================================================================
// Preset/program discovery and accessors
// ====================================================================

void VST3Effect::discoverPresets() {
    programLists_.clear();
    filePresetsByBank_.clear();
    embeddedXmlByBank_.clear();
    compressedPresets_.clear();
    usingFilePresets_ = false;
    usingEmbeddedPresets_ = false;
    usingCompressedPresets_ = false;
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
            for (int p = 0; p < (int)parameters_.size(); p++) {
                if (parameters_[p].paramId == info.id) {
                    programChangeParamIdx_ = p;
                    break;
                }
            }
            
            break;
        }
    }

    // Try to get IUnitInfo from the edit controller
    IUnitInfo* unitInfo = nullptr;
    tresult res = ctrl->queryInterface(IUnitInfo::iid, (void**)&unitInfo);
    if (res != kResultOk || !unitInfo) {
        // No IUnitInfo — try file-based presets
        discoverPresetFiles();
        if (!usingFilePresets_) {
            discoverHardcodedPresets();
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
        str128ToChar(plInfo.name, nameBuf, 128);
        pl.name = nameBuf;

        for (int32 pi = 0; pi < plInfo.programCount; pi++) {
            String128 progName;
            memset(progName, 0, sizeof(progName));
            if (unitInfo->getProgramName(plInfo.id, pi, progName) == kResultOk) {
                char progBuf[128];
                str128ToChar(progName, progBuf, 128);
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

    // If this plugin has a known file-preset mapping, prefer file-based
    // presets over IUnitInfo programs (JUCE-based plugins like Valhalla
    // often expose only empty "Init" slots via IUnitInfo).
    bool hasKnownMapping = false;
    for (int i = 0; knownFxPresetMappings[i].pluginNameSubstring != nullptr; i++) {
        if (strstr(name_, knownFxPresetMappings[i].pluginNameSubstring) != nullptr) {
            hasKnownMapping = true;
            break;
        }
    }
    if (hasKnownMapping) {
        discoverPresetFiles();
    }

    // If file scanning didn't find presets, check if IUnitInfo had useful ones
    if (!usingFilePresets_) {
        // Always try hardcoded/embedded presets first for plugins we have
        // embedded data for (like Valhalla). The embedded approach uses
        // setState with actual preset XML, which is far more reliable than
        // kIsProgramChange for JUCE-based plugins.
        discoverHardcodedPresets();
        if (usingEmbeddedPresets_ || usingCompressedPresets_) {
            // Embedded/compressed presets loaded successfully — use them
            return;
        }

        bool hasUsefulPresets = false;
        for (size_t i = 0; i < programLists_.size(); i++) {
            if (programLists_[i].programs.size() > 1) {
                hasUsefulPresets = true;
                break;
            }
        }
        if (!hasUsefulPresets) {
            // No file presets and no useful IUnitInfo presets — try anyway
            discoverPresetFiles();
        }
    }
}

// ====================================================================
// discoverHardcodedPresets: built-in factory preset names for plugins
// whose IUnitInfo may not work reliably on all platforms.
// ====================================================================

// Helper: populate programLists_ and embeddedXmlByBank_ from a
// ValhallaEmbeddedBank array (generated header data).
static void loadValhallaEmbedded(
    const ValhallaEmbeddedBank *banks, int bankCount,
    std::vector<VST3ProgramList> &programLists,
    std::vector<std::vector<const char *>> &embeddedXml)
{
    programLists.clear();
    embeddedXml.clear();
    for (int b = 0; b < bankCount; b++) {
        VST3ProgramList pl;
        pl.listId = b;
        pl.name = banks[b].bankName;
        std::vector<const char *> xmlPtrs;
        for (int p = 0; p < banks[b].presetCount; p++) {
            pl.programs.push_back(banks[b].presets[p].name);
            xmlPtrs.push_back(banks[b].presets[p].xmlData);
        }
        programLists.push_back(pl);
        embeddedXml.push_back(xmlPtrs);
    }
}

void VST3Effect::discoverHardcodedPresets() {
    // --- TAL-Reverb-4: embedded XML + setState ---
    // kIsProgramChange works on desktop but fails on Steam Deck.
    // Use the same reliable setState approach as Valhalla/NoiseMaker.
    if (strstr(name_, "TAL-Reverb") != nullptr || strstr(name_, "Reverb-4") != nullptr ||
        strstr(name_, "TAL Reverb") != nullptr) {
        programLists_.clear();
        embeddedXmlByBank_.clear();
        VST3ProgramList pl;
        pl.listId = 0;
        pl.name = "Factory Presets";
        std::vector<const char *> xmlPtrs;
        for (int p = 0; p < TAL_REVERB4_EMBEDDED_COUNT; p++) {
            pl.programs.push_back(talReverb4EmbeddedPresets[p].name);
            xmlPtrs.push_back(talReverb4EmbeddedPresets[p].xmlData);
        }
        programLists_.push_back(pl);
        embeddedXmlByBank_.push_back(xmlPtrs);
        usingEmbeddedPresets_ = true;
        currentBank_ = 0;
        currentPreset_ = 0;
        return;
    }

    // --- Valhalla DSP effects: embedded .vpreset XML data ---
    struct ValhallaMapping {
        const char *nameSubstring;
        const ValhallaEmbeddedBank *banks;
        int bankCount;
    };
    static const ValhallaMapping valhallaMappings[] = {
        { "ValhallaDelay",          valhallaDelayBanks,          VALHALLA_DELAY_BANK_COUNT },
        { "ValhallaPlate",          valhallaPlateBanks,          VALHALLA_PLATE_BANK_COUNT },
        { "ValhallaRoom",           valhallaRoomBanks,           VALHALLA_ROOM_BANK_COUNT },
        { "ValhallaSpaceModulator", valhallaSpaceModulatorBanks, VALHALLA_SPACEMODULATOR_BANK_COUNT },
        { "ValhallaSupermassive",   valhallaSupermassiveBanks,   VALHALLA_SUPERMASSIVE_BANK_COUNT },
        { "ValhallaUberMod",        valhallaUberModBanks,        VALHALLA_UBERMOD_BANK_COUNT },
        { "ValhallaVintageVerb",    valhallaVintageVerbBanks,    VALHALLA_VINTAGEVERB_BANK_COUNT },
        { nullptr, nullptr, 0 }
    };

    for (int i = 0; valhallaMappings[i].nameSubstring != nullptr; i++) {
        if (strstr(name_, valhallaMappings[i].nameSubstring) != nullptr) {
            loadValhallaEmbedded(
                valhallaMappings[i].banks,
                valhallaMappings[i].bankCount,
                programLists_,
                embeddedXmlByBank_);
            usingEmbeddedPresets_ = true;
            currentBank_ = 0;
            currentPreset_ = 0;
            return;
        }
    }

    // --- Arturia Efx FRAGMENTS: compressed binary state data ---
    // PCHK component states extracted from .nksf files and zlib-compressed.
    // On Steam Deck the Wine prefix may not have the preset files installed,
    // so we bundle them.
    if (strstr(name_, "FRAGMENTS") != nullptr || strstr(name_, "Efx FRAGMENTS") != nullptr) {
        programLists_.clear();
        compressedPresets_.clear();
        VST3ProgramList pl;
        pl.listId = 0;
        pl.name = "Factory Presets";
        for (int p = 0; p < EFX_FRAGMENTS_PRESET_COUNT; p++) {
            pl.programs.push_back(efxFragmentsEmbeddedPresets[p].name);
            CompressedPreset cp;
            cp.data = efxFragmentsEmbeddedPresets[p].compressedData;
            cp.compressedSize = efxFragmentsEmbeddedPresets[p].compressedSize;
            cp.originalSize = efxFragmentsEmbeddedPresets[p].originalSize;
            compressedPresets_.push_back(cp);
        }
        programLists_.push_back(pl);
        usingCompressedPresets_ = true;
        currentBank_ = 0;
        currentPreset_ = 0;
        return;
    }
}

const char *VST3Effect::GetBankName(int bankIdx) const {
    if (bankIdx >= 0 && bankIdx < (int)programLists_.size()) {
        return programLists_[bankIdx].name.c_str();
    }
    return "---";
}

void VST3Effect::SetCurrentBank(int bankIdx) {
    if (bankIdx >= 0 && bankIdx < (int)programLists_.size()) {
        currentBank_ = bankIdx;
        currentPreset_ = 0;
    }
}

int VST3Effect::GetPresetCount() const {
    if (currentBank_ >= 0 && currentBank_ < (int)programLists_.size()) {
        return (int)programLists_[currentBank_].programs.size();
    }
    return 0;
}

const char *VST3Effect::GetPresetName(int presetIdx) const {
    if (currentBank_ >= 0 && currentBank_ < (int)programLists_.size()) {
        const VST3ProgramList &pl = programLists_[currentBank_];
        if (presetIdx >= 0 && presetIdx < (int)pl.programs.size()) {
            return pl.programs[presetIdx].c_str();
        }
    }
    return "---";
}

void VST3Effect::SetPreset(int presetIdx) {
    if (currentBank_ < 0 || currentBank_ >= (int)programLists_.size()) return;
    const VST3ProgramList &pl = programLists_[currentBank_];
    if (presetIdx < 0 || presetIdx >= (int)pl.programs.size()) return;

    currentPreset_ = presetIdx;

    // --- Embedded preset loading (Valhalla/TAL plugins compiled into binary) ---
    if (usingEmbeddedPresets_) {
        if (currentBank_ < (int)embeddedXmlByBank_.size() &&
            presetIdx < (int)embeddedXmlByBank_[currentBank_].size()) {
            loadPresetFromXmlData(embeddedXmlByBank_[currentBank_][presetIdx]);
        }
        return;
    }

    // --- Compressed binary preset loading (Efx FRAGMENTS) ---
    if (usingCompressedPresets_) {
        if (presetIdx < (int)compressedPresets_.size()) {
            const CompressedPreset &cp = compressedPresets_[presetIdx];
            loadPresetFromCompressedData(cp.data, cp.compressedSize, cp.originalSize);
        }
        return;
    }

    // --- File-based preset loading ---
    if (usingFilePresets_) {
        if (currentBank_ < (int)filePresetsByBank_.size() &&
            presetIdx < (int)filePresetsByBank_[currentBank_].size()) {

            const VST3FilePreset &fp = filePresetsByBank_[currentBank_][presetIdx];

            int headerSkip = 0;
            for (int m = 0; knownFxPresetMappings[m].pluginNameSubstring != nullptr; m++) {
                if (strstr(name_, knownFxPresetMappings[m].pluginNameSubstring) != nullptr) {
                    headerSkip = knownFxPresetMappings[m].headerSkipBytes;
                    break;
                }
            }
            loadPresetFromFile(fp.filePath, headerSkip);
        }
        return;
    }

    if (!editController_) return;
    IEditController* ctrl = (IEditController*)editController_;

    // Select preset via kIsProgramChange parameter
    int32 pCount = ctrl->getParameterCount();
    for (int32 i = 0; i < pCount; i++) {
        ParameterInfo info;
        if (ctrl->getParameterInfo(i, info) != kResultOk) continue;
        if (!(info.flags & ParameterInfo::kIsProgramChange)) continue;

        double normalized = 0.0;
        if (info.stepCount > 0) {
            normalized = (double)presetIdx / (double)info.stepCount;
        }
        if (normalized < 0.0) normalized = 0.0;
        if (normalized > 1.0) normalized = 1.0;

        ctrl->setParamNormalized(info.id, normalized);

        PendingParamChange pc;
        pc.paramId = info.id;
        pc.value = normalized;
        pendingEventsMutex_.Lock();
        pendingParamChanges_.push_back(pc);
        pendingEventsMutex_.Unlock();

        // Update parameter variable if it exists
        for (int p = 0; p < (int)parameters_.size(); p++) {
            if (parameters_[p].paramId == info.id && parameters_[p].variable) {
                int maxVal = (info.stepCount > 0 && info.stepCount <= 255) ? info.stepCount : 127;
                int intVal = (int)(normalized * maxVal);
                parameters_[p].variable->SetInt(intVal);
                parameters_[p].currentValue = normalized;
                break;
            }
        }

        
        break;
    }
}

void VST3Effect::cleanupPlugin() {
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
    if (editController_ && !componentIsController_) {
        IEditController* ctrl = (IEditController*)editController_;
        ctrl->terminate();
        ctrl->release();
    }
    editController_ = nullptr;
    if (audioProcessor_) {
        IAudioProcessor* proc = (IAudioProcessor*)audioProcessor_;
        proc->release();
        audioProcessor_ = nullptr;
    }
    if (component_) {
        IComponent* comp = (IComponent*)component_;
        comp->terminate();
        comp->release();
        component_ = nullptr;
    }
    if (moduleHandle_) {
        VST3FXModuleExitFunc moduleExit = (VST3FXModuleExitFunc)dlsym(moduleHandle_, "ModuleExit");
        if (moduleExit) moduleExit();
        dlclose(moduleHandle_);
        moduleHandle_ = nullptr;
    }
    pluginFactory_ = nullptr;
    componentIsController_ = false;

    free(audioBufferL_); audioBufferL_ = nullptr;
    free(audioBufferR_); audioBufferR_ = nullptr;
    free(audioInputL_); audioInputL_ = nullptr;
    free(audioInputR_); audioInputR_ = nullptr;
    bufferSize_ = 0;
}

void VST3Effect::setupProcessing(int bufferSize) {
    if (!audioProcessor_ || !component_) return;

    IAudioProcessor* proc = (IAudioProcessor*)audioProcessor_;
    IComponent* comp = (IComponent*)component_;

    int sampleRate = Audio::GetInstance()->GetSampleRate();
    if (sampleRate <= 0) sampleRate = 44100;

    if (proc->canProcessSampleSize(kSample32) != kResultOk) {
        Trace::Error("VST3FX: Plugin does not support 32-bit float");
        return;
    }

    SpeakerArrangement stereo = 0x3;
    int32 numInputBuses = comp->getBusCount(kAudio, kInput);
    int32 numOutputBuses = comp->getBusCount(kAudio, kOutput);

    // Activate bus 0 for input/output, explicitly deactivate all others.
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

    // Zero-init to avoid garbage in padding bytes that yabridge may
    // serialize across the IPC boundary.
    ProcessSetup setup;
    memset(&setup, 0, sizeof(setup));
    setup.processMode = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = bufferSize;
    setup.sampleRate = (double)sampleRate;

    tresult res = proc->setupProcessing(setup);
    if (res != kResultOk) {
        Trace::Error("VST3FX: setupProcessing failed");
        return;
    }

    res = comp->setActive(true);
    if (res != kResultOk) {
        Trace::Error("VST3FX: setActive(true) failed");
        return;
    }
    isActive_ = true;

    res = proc->setProcessing(true);
    if (res != kResultOk) {
        
    }
    isProcessing_ = true;
}

bool VST3Effect::ProcessAudio(fixed *buffer, int sampleCount, int wetDry) {
    if (!pluginMutex_.TryLock()) {
        return true;  // pass through dry
    }

    if (!audioProcessor_ || !component_) {
        pluginMutex_.Unlock();
        return false;
    }

    if (!buffer || sampleCount <= 0) {
        pluginMutex_.Unlock();
        return false;
    }

    IAudioProcessor* proc = (IAudioProcessor*)audioProcessor_;
    IComponent* comp = (IComponent*)component_;

    // Allocate / resize buffers
    if (sampleCount > bufferSize_) {
        free(audioBufferL_); free(audioBufferR_);
        free(audioInputL_); free(audioInputR_);
        audioBufferL_ = (float*)calloc(sampleCount, sizeof(float));
        audioBufferR_ = (float*)calloc(sampleCount, sizeof(float));
        audioInputL_ = (float*)calloc(sampleCount, sizeof(float));
        audioInputR_ = (float*)calloc(sampleCount, sizeof(float));
        bufferSize_ = sampleCount;

        if (isProcessing_) {
            proc->setProcessing(false);
            isProcessing_ = false;
        }
        if (isActive_) {
            comp->setActive(false);
            isActive_ = false;
        }
        setupProcessing(sampleCount);
    }

    if (!isActive_ || !isProcessing_) {
        pluginMutex_.Unlock();
        return false;
    }

    // Deinterleave fixed-point → float (same as LV2Effect)
    const float toFloat = 1.0f / (32768.0f * 32768.0f);
    for (int i = 0; i < sampleCount; i++) {
        audioInputL_[i] = (float)buffer[i * 2] * toFloat;
        audioInputR_[i] = (float)buffer[i * 2 + 1] * toFloat;
    }

    memset(audioBufferL_, 0, sampleCount * sizeof(float));
    memset(audioBufferR_, 0, sampleCount * sizeof(float));

    // Sync parameter changes
    FXParameterChanges paramChanges;
    pendingEventsMutex_.Lock();
    renderLocalParams_.swap(pendingParamChanges_);
    pendingEventsMutex_.Unlock();

    for (size_t i = 0; i < parameters_.size(); ++i) {
        VST3PluginParameter& p = parameters_[i];
        if (p.variable && !p.isReadOnly) {
            int varVal = p.variable->GetInt();
            int maxVal = (p.stepCount > 0 && p.stepCount <= 255) ? p.stepCount : 127;
            double norm = (double)varVal / (double)maxVal;
            if (norm < 0.0) norm = 0.0;
            if (norm > 1.0) norm = 1.0;

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

    for (auto& pc : renderLocalParams_) {
        int32 idx;
        IParamValueQueue* queue = paramChanges.addParameterData(pc.paramId, idx);
        if (queue) {
            int32 ptIdx;
            queue->addPoint(0, pc.value, ptIdx);
        }
    }
    renderLocalParams_.clear();

    // Set up ProcessData
    ProcessData processData;
    processData.processMode = kRealtime;
    processData.symbolicSampleSize = kSample32;
    processData.numSamples = sampleCount;

    AudioBusBuffers inputBus;
    float* inputChannels[2] = { audioInputL_, audioInputR_ };
    inputBus.numChannels = 2;
    inputBus.channelBuffers32 = inputChannels;
    inputBus.silenceFlags = 0;

    AudioBusBuffers outputBus;
    float* outputChannels[2] = { audioBufferL_, audioBufferR_ };
    outputBus.numChannels = 2;
    outputBus.channelBuffers32 = outputChannels;
    outputBus.silenceFlags = 0;

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

    FXEventList eventList;
    processData.inputEvents = &eventList;
    processData.inputParameterChanges = &paramChanges;
    processData.outputParameterChanges = nullptr;
    processData.outputEvents = nullptr;
    processData.processContext = nullptr;

    // Run the plugin
    proc->process(processData);

    // Sanitise NaN/Inf
    {
        union { float f; uint32_t u; } chk;
        for (int i = 0; i < sampleCount; i++) {
            chk.f = audioBufferL_[i];
            if ((chk.u & 0x7F800000u) == 0x7F800000u) audioBufferL_[i] = 0.0f;
            chk.f = audioBufferR_[i];
            if ((chk.u & 0x7F800000u) == 0x7F800000u) audioBufferR_[i] = 0.0f;
        }
    }

    // Wet/dry mix and convert back to fixed-point
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

void VST3Effect::Update(Observable &o, I_ObservableData *d) {
    // No-op: audio thread reads Variables directly each cycle
}

void VST3Effect::StorePendingVariable(const char *name, const char *value) {
    pendingParamValues_[name] = value;
}

void VST3Effect::SaveContent(TiXmlNode *node) {
    TiXmlElement effect("EFFECT");
    effect.SetAttribute("TYPE", "VST3");
    effect.SetAttribute("PATH", pluginPath_);
    effect.SetAttribute("CID", tuidHex(pluginClassId_).c_str());

    for (size_t i = 0; i < parameters_.size(); i++) {
        if (parameters_[i].variable) {
            TiXmlElement param("PARAM");
            param.SetAttribute("NAME", parameters_[i].variable->GetName());
            param.SetAttribute("VALUE", parameters_[i].variable->GetInt());
            effect.InsertEndChild(param);
        }
    }

    Variable *vol = FindVariable(VST3FX_VOLUME);
    if (vol) {
        TiXmlElement v("VOL");
        v.SetAttribute("VALUE", vol->GetInt());
        effect.InsertEndChild(v);
    }
    Variable *wet = FindVariable(VST3FX_WETDRY);
    if (wet) {
        TiXmlElement w("WET");
        w.SetAttribute("VALUE", wet->GetInt());
        effect.InsertEndChild(w);
    }

    // Save bank/preset if available
    if (!programLists_.empty()) {
        TiXmlElement bp("BANKPRESET");
        bp.SetAttribute("BANK", currentBank_);
        bp.SetAttribute("PRESET", currentPreset_);
        effect.InsertEndChild(bp);
    }

    node->InsertEndChild(effect);
}

void VST3Effect::RestoreContent(TiXmlElement *element) {
    const char *path = element->Attribute("PATH");
    const char *cid = element->Attribute("CID");
    if (!path || path[0] == '\0' || !cid) return;

    // Store pending params
    TiXmlElement *paramEl = element->FirstChildElement("PARAM");
    while (paramEl) {
        const char *name = paramEl->Attribute("NAME");
        const char *value = paramEl->Attribute("VALUE");
        if (name && value) StorePendingVariable(name, value);
        paramEl = paramEl->NextSiblingElement("PARAM");
    }

    // Restore volume and wet
    TiXmlElement *volEl = element->FirstChildElement("VOL");
    if (volEl) {
        const char *val = volEl->Attribute("VALUE");
        if (val) {
            Variable *v = FindVariable(VST3FX_VOLUME);
            if (v) v->SetInt(atoi(val));
        }
    }
    TiXmlElement *wetEl = element->FirstChildElement("WET");
    if (wetEl) {
        const char *val = wetEl->Attribute("VALUE");
        if (val) {
            Variable *v = FindVariable(VST3FX_WETDRY);
            if (v) v->SetInt(atoi(val));
        }
    }

    uint8_t classId[16];
    hexTuid(cid, classId);
    SetPlugin(path, classId);

    // Restore bank/preset if saved
    TiXmlElement *bpEl = element->FirstChildElement("BANKPRESET");
    if (bpEl) {
        int bank = 0, preset = 0;
        bpEl->Attribute("BANK", &bank);
        bpEl->Attribute("PRESET", &preset);
        if (bank >= 0 && bank < (int)programLists_.size()) {
            SetCurrentBank(bank);
            SetPreset(preset);
        }
    }
}

// ====================================================================
// ScanEffectPlugins: find VST3 effect plugins (Fx category, NOT Instrument)
// ====================================================================
// Try to parse a .vst3 bundle's Contents/moduleinfo.json (VST 3.7.5+) to get
// plugin class info without dlopen'ing the module. This is essential for
// yabridge-bridged bundles where dlopen would trigger a Wine launch.
// Returns true if moduleinfo.json was found (even if no matching classes).
static bool tryParseModuleInfoFX(const std::string& bundlePath,
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

    size_t classesPos = json.find("\"Classes\"");
    if (classesPos == std::string::npos) return true;

    size_t arrStart = json.find('[', classesPos);
    if (arrStart == std::string::npos) return true;

    size_t pos = arrStart + 1;
    while (pos < json.size()) {
        size_t objStart = json.find('{', pos);
        if (objStart == std::string::npos) break;
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
            if (strcmp(filterCategory, "Fx") == 0) {
                matches = sub.find("Fx") != std::string::npos
                          && sub.find("Instrument") == std::string::npos;
            } else if (strcmp(filterCategory, "Instrument") == 0) {
                matches = sub.find("Instrument") != std::string::npos;
            }
            if (sub.empty()) matches = true;

            if (matches) {
                if (name.find("Steam") == std::string::npos &&
                    name.find("Valve") == std::string::npos) {
                    VST3PluginInfo info;
                    info.name = name;
                    info.path = bundlePath;
                    info.classIdStr = cid;
                    hexTuid(cid, info.classId);
                    result.push_back(info);
                }
            }
        }
        pos = objEnd + 1;
    }
    return true;
}

std::vector<VST3PluginInfo> VST3Effect::ScanEffectPlugins() {
    std::vector<VST3PluginInfo> result;

    std::vector<std::string> searchPaths;
    const char* home = getenv("HOME");
    if (home) searchPaths.push_back(std::string(home) + "/.vst3");
    searchPaths.push_back("/usr/lib/vst3");
    searchPaths.push_back("/usr/local/lib/vst3");

    std::vector<std::string> bundles;
    for (auto& sp : searchPaths) {
        findBundles(sp, bundles);
    }

    for (auto& bundlePath : bundles) {
        // Try moduleinfo.json first (fast, no loading / Wine needed)
        if (tryParseModuleInfoFX(bundlePath, "Fx", result)) {
            continue;
        }

        // Skip yabridge bridge bundles that lack moduleinfo.json — dlopen'ing
        // them would trigger the chainloader which needs Wine and shows a
        // desktop error notification if libyabridge-vst3.so isn't reachable.
        if (bundlePath.find("/yabridge/") != std::string::npos) {
            continue;
        }

        // Fall back to dlopen for native plugins without moduleinfo.json
        std::string soPath = resolveSoPath(bundlePath);
        if (soPath.empty()) continue;

        void* handle = dlopen(soPath.c_str(), RTLD_LAZY);
        if (!handle) continue;

        VST3FXModuleEntryFunc entry = (VST3FXModuleEntryFunc)dlsym(handle, "ModuleEntry");
        VST3FXModuleExitFunc exit_fn = (VST3FXModuleExitFunc)dlsym(handle, "ModuleExit");
        VST3FXGetFactoryFunc getFactory = (VST3FXGetFactoryFunc)dlsym(handle, "GetPluginFactory");

        if (!entry || !exit_fn || !getFactory) { dlclose(handle); continue; }
        if (!entry(handle)) { dlclose(handle); continue; }

        IPluginFactory* factory = getFactory();
        if (factory) {
            int count = factory->countClasses();

            IPluginFactory2* factory2 = nullptr;
            factory->queryInterface(IPluginFactory2::iid, (void**)&factory2);

            for (int i = 0; i < count; i++) {
                PClassInfo ci;
                if (factory->getClassInfo(i, &ci) != kResultOk) continue;
                if (strcmp(ci.category, kVstAudioEffectClass) != 0) continue;

                bool isEffect = false;
                if (factory2) {
                    PClassInfo2 ci2;
                    if (factory2->getClassInfo2(i, &ci2) == kResultOk) {
                        // Include only if it has Fx subcategory and is NOT an instrument
                        if (strstr(ci2.subCategories, "Fx") != nullptr &&
                            strstr(ci2.subCategories, "Instrument") == nullptr) {
                            isEffect = true;
                        }
                    }
                } else {
                    // No factory2 — include all non-instrument plugins
                    isEffect = true;
                }

                if (!isEffect) continue;

                VST3PluginInfo info;
                info.name = ci.name;
                info.path = bundlePath;
                memcpy(info.classId, ci.cid, 16);
                info.classIdStr = tuidHex((const uint8_t*)ci.cid);

                // Skip Steam/Valve system plugins
                if (info.name.find("Steam") != std::string::npos ||
                    info.name.find("Valve") != std::string::npos) {
                    continue;
                }

                result.push_back(info);
            }

            if (factory2) factory2->release();
        }

        exit_fn();
        dlclose(handle);
    }

    std::sort(result.begin(), result.end(),
        [](const VST3PluginInfo& a, const VST3PluginInfo& b) {
            return a.name < b.name;
        });

    return result;
}

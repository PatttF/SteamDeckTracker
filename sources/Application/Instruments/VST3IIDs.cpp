/**
 * VST3IIDs.cpp — VST3 interface IID definitions
 *
 * This file emits the DEF_CLASS_IID symbols for the VST3 interfaces we use.
 * The base IIDs (FUnknown, IPluginFactory, etc.) come from coreiids.cpp in
 * the SDK, and the FUID class implementation comes from funknown.cpp.  This
 * file covers the VST-specific interfaces (IComponent, IAudioProcessor, etc.)
 * that are not covered by coreiids.cpp.
 */

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstunits.h"

namespace Steinberg {

DEF_CLASS_IID (Vst::IComponent)
DEF_CLASS_IID (Vst::IAudioProcessor)
DEF_CLASS_IID (Vst::IEditController)
DEF_CLASS_IID (Vst::IComponentHandler)
DEF_CLASS_IID (Vst::IParamValueQueue)
DEF_CLASS_IID (Vst::IParameterChanges)
DEF_CLASS_IID (Vst::IEventList)
DEF_CLASS_IID (Vst::IHostApplication)
DEF_CLASS_IID (Vst::IConnectionPoint)
DEF_CLASS_IID (Vst::IMessage)
DEF_CLASS_IID (Vst::IAttributeList)
DEF_CLASS_IID (Vst::IUnitInfo)

} // namespace Steinberg

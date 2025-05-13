#include "processor.h"
#include "controller.h"
#include "cids.h"

#include "public.sdk/source/main/pluginfactory.h"

//------------------------------------------------------------------------
//  VST Plug-in Entry
//------------------------------------------------------------------------
// Windows: do not forget to include a .def file in your project to export
// GetPluginFactory function!
//------------------------------------------------------------------------

BEGIN_FACTORY_DEF(
    "Sketch Audio, LLC", // Company name
    "www.sketchaudio.com", // Company website
    "mailto:ryan@sketchaudio.com" // Company email
)

// its kVstAudioEffectClass component
DEF_CLASS2(
    INLINE_UID_FROM_FUID(tiny::processor_uid),
    Steinberg::PClassInfo::kManyInstances,	// cardinality
    kVstAudioEffectClass,	// the component category (do not changed this)
    "Tiny VST3 Demo",		// here the Plug-in name (to be changed)
    Steinberg::Vst::kDistributable,	// means that component and controller could be distributed on different computers
    "Fx", // Subcategory for this Plug-in (to be changed)
    "1.0.0",		// Plug-in version (to be changed)
    kVstVersionString,		// the VST 3 SDK version (do not changed this, use always this define)
    tiny::Processor::createInstance // function pointer called when this component should be instantiated
)	

// its kVstComponentControllerClass component
DEF_CLASS2(
    INLINE_UID_FROM_FUID(tiny::controller_uid),
    Steinberg::PClassInfo::kManyInstances, // cardinality
    kVstComponentControllerClass,// the Controller category (do not changed this)
    "Tiny VST3 Demo",	// controller name (could be the same than component name)
    0,						// not used here
    "",						// not used here
    "1.0.0",		// Plug-in version (to be changed)
    kVstVersionString,		// the VST 3 SDK version (do not changed this, use always this define)
    tiny::Controller::createInstance // function pointer called when this component should be instantiated
)

END_FACTORY
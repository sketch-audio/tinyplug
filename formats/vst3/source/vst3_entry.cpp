#include "public.sdk/source/main/pluginfactory.h"

#include "plug_info.h"

#include "vst3_adapters.h"
#include "vst3_controller.h"
#include "vst3_processor.h"

//------------------------------------------------------------------------
//  VST Plug-in Entry
//------------------------------------------------------------------------
// Windows: do not forget to include a .def file in your project to export
// GetPluginFactory function!
//------------------------------------------------------------------------

BEGIN_FACTORY_DEF(
    tiny::Plug_info::company_name,
    tiny::Plug_info::company_website,
    tiny::Plug_info::company_email
)

    // its kVstAudioEffectClass component
    DEF_CLASS2(
        INLINE_UID_FROM_FUID(tiny::map_to_fuid(tiny::Plug_info::Vst3::processor_uid)),
        Steinberg::PClassInfo::kManyInstances,	            // Cardinality
        kVstAudioEffectClass,	                            // Category (do not change this)
        tiny::Plug_info::plugin_name,		            // Name (to be changed)
        Steinberg::Vst::kDistributable,	                    // Means that component and controller could be distributed on different computers
        tiny::Plug_info::Vst3::subcategories,           // Subcategory for this Plug-in (to be changed)
        tiny::Plug_info::version_string,        	    // Plug-in version (to be changed)
        kVstVersionString,		                            // VST3 SDK version (do not change this, use always this define)
        tiny::Vst3_processor::createInstance                      // Function pointer called when this component should be instantiated
    )	

    // its kVstComponentControllerClass component
    DEF_CLASS2(
        INLINE_UID_FROM_FUID(tiny::map_to_fuid(tiny::Plug_info::Vst3::controller_uid)),
        Steinberg::PClassInfo::kManyInstances,          // Cardinality
        kVstComponentControllerClass,                   // Category (do not change this)
        tiny::Plug_info::plugin_name,	            // Name (could be the same than component name)
        0,						                        // Class flags, not used here
        "",						                        // Subcategories, not used here
        tiny::Plug_info::version_string,        	// Plug-in version (to be changed)
        kVstVersionString,		                        // VST3 SDK version (do not change this, use always this define)
        tiny::Vst3_controller::createInstance                 // Function pointer called when this component should be instantiated
    )

END_FACTORY
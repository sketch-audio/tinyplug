#include "public.sdk/source/main/pluginfactory.h"

#include "cmake_defines.h"
#include "user_plug.h"

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
    tiny::User_plug::info.company_name,
    tiny::User_plug::info.company_website,
    tiny::User_plug::info.company_email
)

    // its kVstAudioEffectClass component
    DEF_CLASS2(
        INLINE_UID_FROM_FUID(tiny::map_to_fuid(tiny::User_plug::info.vst3_processor_uid)),
        Steinberg::PClassInfo::kManyInstances,	            // Cardinality
        kVstAudioEffectClass,	                            // Category (do not change this)
        tiny::Cmake_defines::product_name,		            // Name (to be changed)
        Steinberg::Vst::kDistributable,	                    // Means that component and controller could be distributed on different computers
        tiny::User_plug::info.vst3_subcategories,           // Subcategory for this Plug-in (to be changed)
        tiny::Cmake_defines::version_string,        	    // Plug-in version (to be changed)
        kVstVersionString,		                            // VST3 SDK version (do not change this, use always this define)
        Vst3_processor::createInstance                           // Function pointer called when this component should be instantiated
    )	

    // its kVstComponentControllerClass component
    DEF_CLASS2(
        INLINE_UID_FROM_FUID(tiny::map_to_fuid(tiny::User_plug::info.vst3_controller_uid)),
        Steinberg::PClassInfo::kManyInstances,          // Cardinality
        kVstComponentControllerClass,                   // Category (do not change this)
        tiny::Cmake_defines::product_name,	            // Name (could be the same than component name)
        0,						                        // Class flags, not used here
        "",						                        // Subcategories, not used here
        tiny::Cmake_defines::version_string,        	// Plug-in version (to be changed)
        kVstVersionString,		                        // VST3 SDK version (do not change this, use always this define)
        Vst3_controller::createInstance                      // Function pointer called when this component should be instantiated
    )

END_FACTORY
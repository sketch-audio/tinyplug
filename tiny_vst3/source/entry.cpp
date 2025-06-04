#include "public.sdk/source/main/pluginfactory.h"

#include "cmake_defines.h"
#include "user_plug.h"

#include "adapters.h"
#include "controller.h"
#include "processor.h"

using namespace tiny;

//------------------------------------------------------------------------
//  VST Plug-in Entry
//------------------------------------------------------------------------
// Windows: do not forget to include a .def file in your project to export
// GetPluginFactory function!
//------------------------------------------------------------------------

BEGIN_FACTORY_DEF(
    User_plug::info.company_name.c_str(),
    User_plug::info.company_website.c_str(),
    User_plug::info.company_email.c_str()
)

    // its kVstAudioEffectClass component
    DEF_CLASS2(
        INLINE_UID_FROM_FUID(map_to_fuid(User_plug::info.vst3_processor_uid)),
        Steinberg::PClassInfo::kManyInstances,	        // Cardinality
        kVstAudioEffectClass,	                        // Category (do not change this)
        Cmake_defines::product_name.c_str(),		    // Name (to be changed)
        Steinberg::Vst::kDistributable,	                // Means that component and controller could be distributed on different computers
        User_plug::info.vst3_subcategories.c_str(),     // Subcategory for this Plug-in (to be changed)
        Cmake_defines::version_string.c_str(),	        // Plug-in version (to be changed)
        kVstVersionString,		                        // VST3 SDK version (do not change this, use always this define)
        Processor::createInstance                       // Function pointer called when this component should be instantiated
    )	

    // its kVstComponentControllerClass component
    DEF_CLASS2(
        INLINE_UID_FROM_FUID(map_to_fuid(User_plug::info.vst3_controller_uid)),
        Steinberg::PClassInfo::kManyInstances,          // Cardinality
        kVstComponentControllerClass,                   // Category (do not change this)
        Cmake_defines::product_name.c_str(),	        // Name (could be the same than component name)
        0,						                        // Class flags, not used here
        "",						                        // Subcategories, not used here
        Cmake_defines::version_string.c_str(),	        // Plug-in version (to be changed)
        kVstVersionString,		                        // VST3 SDK version (do not change this, use always this define)
        Controller::createInstance                      // Function pointer called when this component should be instantiated
    )

END_FACTORY
// AAX Includes
#include "AAX_ICollection.h"
#include "AAX_IComponentDescriptor.h"
#include "AAX_IEffectDescriptor.h"
#include "AAX_IPropertyMap.h"
#include "AAX_Exception.h"
#include "AAX_Errors.h"
#include "AAX_Assert.h"

#include "plug_info.h"
#include "models/param_model.h"

#include "aax_categories.h"
#include "aax_gui.h"
#include "aax_parameters.h"

//
AAX_Result GetEffectDescriptions(AAX_ICollection* collection)
{
    using namespace tiny;

    if (auto* descriptor = collection->NewDescriptor()) {

        descriptor->AddName(Plug_info::plugin_name);
        descriptor->AddName(Plug_info::plugin_short_name);
        descriptor->AddCategory(TINY_AAX_CATEGORIES);
        descriptor->AddProcPtr((void*)Aax_parameters::Create, kAAX_ProcPtrID_Create_EffectParameters);
        descriptor->AddProcPtr((void*)Aax_gui::Create, kAAX_ProcPtrID_Create_EffectGUI);

        //
        auto make_setup = [](bool stereo) {
            auto info = AAX_SInstrumentSetupInfo{};
            info.mNeedsTransport = true;
            info.mInputStemFormat = stereo ? AAX_eStemFormat_Stereo : AAX_eStemFormat_Mono;
            info.mOutputStemFormat = stereo ? AAX_eStemFormat_Stereo : AAX_eStemFormat_Mono;
            info.mWantsSidechain = Plug_info::wants_sidechain;
            info.mManufacturerID = Plug_info::Aax::manufacturer_id;
            info.mProductID = Plug_info::Aax::product_id;
            info.mPluginID = Plug_info::Aax::plugin_id + (stereo ? 0 : 1);
            return info;
        };
        
        Aax_parameters::StaticDescribe(descriptor, make_setup(true));

        if constexpr (Plug_info::can_process_mono) {
            Aax_parameters::StaticDescribe(descriptor, make_setup(false));
        }

        //
        collection->AddEffect(Plug_info::base_identifier, descriptor);
        collection->SetManufacturerName(Plug_info::company_name);
        collection->AddPackageName(Plug_info::plugin_name);
        collection->AddPackageName(Plug_info::plugin_short_name);
        collection->SetPackageVersion(Plug_info::build_number);

        return AAX_SUCCESS;
    }

    return AAX_ERROR_NULL_OBJECT;
}
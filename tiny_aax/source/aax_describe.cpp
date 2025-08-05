// AAX Includes
#include "AAX_ICollection.h"
#include "AAX_IComponentDescriptor.h"
#include "AAX_IEffectDescriptor.h"
#include "AAX_IPropertyMap.h"
#include "AAX_Exception.h"
#include "AAX_Errors.h"
#include "AAX_Assert.h"

#include "plug_info.h"
#include "user/param_model.h"

#include "aax_gui.h"
#include "aax_parameters.h"

//
AAX_Result GetEffectDescriptions(AAX_ICollection* collection)
{
    
    if (auto* descriptor = collection->NewDescriptor()) {

        descriptor->AddName(tiny::Plug_info::product_name);
        descriptor->AddName(tiny::Plug_info::product_short_name);
        descriptor->AddCategory(AAX_ePlugInCategory_None); // TODO: - 
        descriptor->AddProcPtr((void*)Aax_parameters::Create, kAAX_ProcPtrID_Create_EffectParameters);
        descriptor->AddProcPtr((void*)Aax_gui::Create, kAAX_ProcPtrID_Create_EffectGUI);

        //
        auto info = tiny::aax::AAX_SInstrumentSetupInfo{};
        info.mNeedsTransport = true;
        info.mInputStemFormat = AAX_eStemFormat_Stereo;
        info.mOutputStemFormat = AAX_eStemFormat_Stereo;
        info.mWantsSidechain = tiny::Plug_info::wants_sidechain;
        info.mManufacturerID = tiny::Plug_info::Aax::manufacturer_id;
        info.mProductID = tiny::Plug_info::Aax::product_id;
        info.mPluginID = tiny::Plug_info::Aax::plugin_id;
        Aax_parameters::StaticDescribe(descriptor, info);

        //
        collection->AddEffect(tiny::Plug_info::base_identifier, descriptor);
        collection->SetManufacturerName(tiny::Plug_info::company_name);
        collection->AddPackageName(tiny::Plug_info::product_name);
        collection->AddPackageName(tiny::Plug_info::product_short_name);
        collection->SetPackageVersion(tiny::Plug_info::build_number);

        return AAX_SUCCESS;
    }

    return AAX_ERROR_NULL_OBJECT;
}
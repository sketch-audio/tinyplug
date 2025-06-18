// AAX Includes
#include "AAX_ICollection.h"
#include "AAX_IComponentDescriptor.h"
#include "AAX_IEffectDescriptor.h"
#include "AAX_IPropertyMap.h"
#include "AAX_Exception.h"
#include "AAX_Errors.h"
#include "AAX_Assert.h"

#include "cmake_defines.h"
#include "user_plug.h"

#include "aax_algorithm.h"
#include "aax_gui.h"
#include "aax_parameters.h"

//
AAX_Result GetEffectDescriptions(AAX_ICollection* collection)
{
    if (auto* descriptor = collection->NewDescriptor()) {
        //
        descriptor->AddName(tiny::Cmake_defines::product_name.c_str());
        descriptor->AddName(tiny::Cmake_defines::product_short_name.c_str());
        descriptor->AddCategory(AAX_ePlugInCategory_None); // TODO: - 
        descriptor->AddProcPtr((void*)Aax_parameters::Create, kAAX_ProcPtrID_Create_EffectParameters);
        descriptor->AddProcPtr((void*)Aax_gui::Create, kAAX_ProcPtrID_Create_EffectGUI);

        auto* component = descriptor->NewComponentDescriptor();
        component->AddDataInPort(AAX_FIELD_INDEX(Aax_context, bypass), sizeof(int32_t));
        component->AddAudioIn(AAX_FIELD_INDEX(Aax_context, input_channels));
        component->AddAudioOut(AAX_FIELD_INDEX(Aax_context, output_channels));
        component->AddAudioBufferLength(AAX_FIELD_INDEX(Aax_context, buffer_size));
        component->AddPrivateData(AAX_FIELD_INDEX(Aax_context, plugin), sizeof(Aax_parameters*));

        auto* properties = component->NewPropertyMap();
        properties->AddProperty(AAX_eProperty_ManufacturerID, tiny::User_plug::info.aax_manufacturer_id);
        properties->AddProperty(AAX_eProperty_ProductID, tiny::User_plug::info.aax_product_id);
        properties->AddProperty(AAX_eProperty_PlugInID_Native, tiny::User_plug::info.aax_plugin_id);
        properties->AddProperty(AAX_eProperty_CanBypass, true);
        properties->AddProperty(AAX_eProperty_InputStemFormat, AAX_eStemFormat_Stereo);
        properties->AddProperty(AAX_eProperty_OutputStemFormat, AAX_eStemFormat_Stereo);
        //properties->AddProperty(AAX_eProperty_Constraint_Location, AAX_eConstraintLocationMask_DataModel);

        component->AddProcessProc_Native(Aax_algorithm, properties);

        descriptor->AddComponent(component);

        //
        collection->AddEffect(tiny::Cmake_defines::base_identifier.c_str(), descriptor);
        collection->SetManufacturerName(tiny::User_plug::info.company_name.c_str());
        collection->AddPackageName(tiny::Cmake_defines::product_name.c_str());
        collection->AddPackageName(tiny::Cmake_defines::product_short_name.c_str());
        collection->SetPackageVersion(tiny::Cmake_defines::build_number);

        return AAX_SUCCESS;
    }

    return AAX_ERROR_NULL_OBJECT;
}
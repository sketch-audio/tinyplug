#include <string>

// AAX Includes
#include "AAX_ICollection.h"
#include "AAX_IComponentDescriptor.h"
#include "AAX_IEffectDescriptor.h"
#include "AAX_IPropertyMap.h"
#include "AAX_Exception.h"
#include "AAX_Errors.h"
#include "AAX_Assert.h"

#include "plug_info.hpp"
#include "models/params.hpp"

#include "categories.hpp"
#include "alg_context.hpp"
#include "alg_proc.hpp"
#include "direct_data.hpp"
#include "gui.hpp"
#include "parameters.hpp"

namespace {

using namespace tiny;
using namespace tiny::aax;

/*
    Describe the algorithm component.

    Every pointer slot in Alg_context must be registered with something, or the host
    will corrupt the context — so slots that a given configuration does not use get a
    small private data block as a filler (the same trick the SDK's own monolithic
    convenience layer uses).
*/
auto describe_algorithm(AAX_IComponentDescriptor* desc, bool stereo) -> void
{
    AAX_CheckedResult err;

    // Host-provided environment.
    err = desc->AddAudioIn(field_audio_in);
    err = desc->AddAudioOut(field_audio_out);
    err = desc->AddAudioBufferLength(field_num_frames);
    err = desc->AddSampleRate(field_sample_rate);
    err = desc->AddMIDINode(field_transport, AAX_eMIDINodeType_Transport, "Transport", 0xffff);

#if TINY_WANTS_SIDECHAIN
    err = desc->AddSideChainIn(field_sidechain);
#endif

    // Inbound packets. All buffered: the host timestamps them and splits render
    // buffers (to a 32-sample minimum on Native) so each lands at its automation
    // breakpoint. That host-managed synchronization is the point of being decoupled.
    err = desc->AddDataInPort(field_runtime, sizeof(Runtime_packet));
    for (auto segment = size_t{}; segment < num_segments; ++segment) {
        err = desc->AddDataInPort(coef_field(segment), sizeof(Coef_segment));
    }

    // Private data. `reset_state` is host-filled at every reset via ResetFieldData; the
    // rest is algorithm-owned and persists across one, with Alg_state holding the user's
    // DSP kernel, constructed by the instance-init callback in this memory space.
    err = desc->AddPrivateData(field_reset_state, static_cast<int32_t>(sizeof(Reset_state)), AAX_ePrivateDataOptions_External);
    err = desc->AddPrivateData(field_state, static_cast<int32_t>(sizeof(Alg_state)), AAX_ePrivateDataOptions_External);
    err = desc->AddPrivateData(field_returns, static_cast<int32_t>(sizeof(Return_ring)), AAX_ePrivateDataOptions_External);
    err = desc->AddPrivateData(field_inbound, static_cast<int32_t>(sizeof(Inbound_ring)), AAX_ePrivateDataOptions_External);

    auto* const properties = desc->NewPropertyMap();
    if (!properties) {
        err = AAX_ERROR_NULL_OBJECT;
        return;
    }

    const auto stem_format = stereo ? AAX_eStemFormat_Stereo : AAX_eStemFormat_Mono;
    err = properties->AddProperty(AAX_eProperty_ManufacturerID, static_cast<int32_t>(Plug_info::Aax::manufacturer_id));
    err = properties->AddProperty(AAX_eProperty_ProductID, static_cast<int32_t>(Plug_info::Aax::product_id));
    err = properties->AddProperty(AAX_eProperty_PlugInID_Native, static_cast<int32_t>(Plug_info::Aax::plugin_id + (stereo ? 0 : 1)));
    err = properties->AddProperty(AAX_eProperty_InputStemFormat, static_cast<int32_t>(stem_format));
    err = properties->AddProperty(AAX_eProperty_OutputStemFormat, static_cast<int32_t>(stem_format));
    err = properties->AddProperty(AAX_eProperty_CanBypass, true);
    err = properties->AddProperty(AAX_eProperty_UsesTransport, true);
    err = properties->AddProperty(AAX_eProperty_ObservesTransportState, true); // So we can get recording state.

    if constexpr (Plug_info::wants_sidechain) {
        err = properties->AddProperty(AAX_eProperty_SupportsSideChainInput, true);
    }

    // No location constraint: the algorithm reaches the data model only through
    // packets in and the Direct Data return channel out, so it does not need to be
    // co-located. (A plug-in declaring buffers would need to add the constraint back
    // — see plans/aax-two-component.md.)

    err = desc->AddProcessProc_Native(
        stereo ? alg_render_stereo : alg_render_mono,
        properties,
        alg_init
    );
}

auto describe_effect(AAX_IEffectDescriptor* descriptor) -> AAX_Result
{
    AAX_CheckedResult err;

    err = descriptor->AddName(Plug_info::plugin_name);
    err = descriptor->AddName(Plug_info::plugin_short_name);
    err = descriptor->AddCategory(TINY_AAX_CATEGORIES);

    // Effect modules: data model, GUI, and the Direct Data return channel.
    err = descriptor->AddProcPtr((void*)Parameters::Create, kAAX_ProcPtrID_Create_EffectParameters);
    err = descriptor->AddProcPtr((void*)Gui::Create, kAAX_ProcPtrID_Create_EffectGUI);
    err = descriptor->AddProcPtr((void*)Direct_data::Create, kAAX_ProcPtrID_Create_EffectDirectData);

#if TINY_AAX_PAGE_TABLE
    const auto table_name = std::string{Plug_info::base_file_name} + "Pages.xml";
    err = descriptor->AddResourceInfo(AAX_eResourceType_PageTable, table_name.c_str());
#endif

    // One algorithm component per stem format.
    const auto add_component = [&](bool stereo) {
        auto* const component = descriptor->NewComponentDescriptor();
        if (!component) {
            err = AAX_ERROR_NULL_OBJECT;
            return;
        }
        describe_algorithm(component, stereo);
        err = descriptor->AddComponent(component);
    };

    add_component(true);
    if constexpr (Plug_info::can_process_mono) {
        add_component(false);
    }

    return err;
}

} // namespace

AAX_Result GetEffectDescriptions(AAX_ICollection* collection)
{
    using namespace tiny;

    AAX_CheckedResult err;

    auto* const descriptor = collection->NewDescriptor();
    if (!descriptor) {
        return AAX_ERROR_NULL_OBJECT;
    }

    AAX_SWALLOW_MULT(
        err = describe_effect(descriptor);
        err = collection->AddEffect(Plug_info::base_identifier, descriptor);
    );

    err = collection->SetManufacturerName(Plug_info::company_name);
    err = collection->AddPackageName(Plug_info::plugin_name);
    err = collection->AddPackageName(Plug_info::plugin_short_name);
    err = collection->SetPackageVersion(Plug_info::build_number);

    return err;
}

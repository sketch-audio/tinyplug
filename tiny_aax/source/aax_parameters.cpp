#include <cstring>

#include "aax_parameters.h"

#include "AAX_CBinaryTaperDelegate.h"
#include "AAX_CBinaryDisplayDelegate.h"
#include "AAX_CNumberDisplayDelegate.h"
#include "AAX_CStateTaperDelegate.h"
#include "AAX_CStateDisplayDelegate.h"
#include "AAX_CUnitDisplayDelegateDecorator.h"

#include "aax_adapters.h"

AAX_Result Aax_parameters::EffectInit()
{
    using namespace tiny;
    const auto tree = Param_model::build_tree();
    _specs = flatten_tree(tree);
    sort_param_specs_by_id(_specs);

    static const auto flat_map = flatten_tree(tree);
    static const auto identifiers = tiny::aax::flatten_tree_to_ids(tree);

    for (size_t i = 0; i < flat_map.size(); ++i) {
        const auto& param = flat_map[i];
        const auto& identifier = identifiers[i];

        std::visit(Inline_visitor{
            [&](const Bool_semantics& b) {
                auto aax_param = std::unique_ptr<AAX_IParameter>(new AAX_CParameter<bool>(
                    identifier.c_str(),
                    AAX_CString(param.name),
                    b.def_val,
                    AAX_CBinaryTaperDelegate<bool>(),
                    AAX_CBinaryDisplayDelegate<bool>("False", "True"),
                    !param.hidden
                ));
                if (std::strlen(param.short_name) > 0) {
                    aax_param->AddShortenedName(param.short_name);
                }
                aax_param->SetNumberOfSteps(2);
                aax_param->SetType(AAX_eParameterType_Discrete);
                if (aax_param->Automatable()) {
                    AddSynchronizedParameter(*aax_param);
                }
                mParameterManager.AddParameter(aax_param.release());
            },
            [&](const List_semantics& l) {
                const auto num_items = l.labels.size();
                auto aax_param = std::unique_ptr<AAX_IParameter>(new AAX_CParameter<uint32_t>(
                    identifier.c_str(),
                    AAX_CString(param.name),
                    static_cast<uint32_t>(l.def_val),
                    AAX_CStateTaperDelegate<uint32_t>(0, num_items - 1),
                    AAX_CStateDisplayDelegate<uint32_t>(num_items, const_cast<const char**>(l.labels.data()), 0),
                    !param.hidden
                ));
                if (std::strlen(param.short_name) > 0) {
                    aax_param->AddShortenedName(param.short_name);
                }
                aax_param->SetNumberOfSteps(num_items);
                aax_param->SetType(AAX_eParameterType_Discrete);
                if (aax_param->Automatable()) {
                    AddSynchronizedParameter(*aax_param);
                }
                mParameterManager.AddParameter(aax_param.release());
            },
            [&](const Float_semantics& f) {
                using TaperDelegate = tiny::aax::FloatSemanticsTaperDelegate<double>;
                using DisplayDelegate = AAX_CNumberDisplayDelegate<double, 1, 1>; // precision: 1, space after: 1
                const auto units_str = units_string(f.units);

                auto aax_param = std::unique_ptr<AAX_IParameter>(new AAX_CParameter<double>(
                    identifier.c_str(),
                    AAX_CString(param.name),
                    f.def_val,
                    TaperDelegate(f), // So we can use our own control adapter.
                    AAX_CUnitDisplayDelegateDecorator<double>(DisplayDelegate(), units_str.c_str()),
                    !param.hidden
                ));
                if (std::strlen(param.short_name) > 0) {
                    aax_param->AddShortenedName(param.short_name);
                }
                aax_param->SetNumberOfSteps(2048); // Is this the most we can have?
                aax_param->SetType(AAX_eParameterType_Continuous);
                if (aax_param->Automatable()) {
                    AddSynchronizedParameter(*aax_param);
                }
                mParameterManager.AddParameter(aax_param.release());
            },
            [&](const Int_semantics& i) {
                using DisplayDelegate = AAX_CNumberDisplayDelegate<int32_t, 0, 1>; // precision: 0, space after: 1
                const auto units_str = units_string(i.units);

                auto aax_param = std::unique_ptr<AAX_IParameter>(new AAX_CParameter<int32_t>(
                    identifier.c_str(),
                    AAX_CString(param.name),
                    i.def_val,
                    AAX_CStateTaperDelegate<int32_t>(i.min_val, i.max_val),
                    AAX_CUnitDisplayDelegateDecorator<int32_t>(DisplayDelegate(), units_str.c_str()),
                    !param.hidden
                ));
                if (std::strlen(param.short_name) > 0) {
                    aax_param->AddShortenedName(param.short_name);
                }
                aax_param->SetNumberOfSteps(i.max_val - i.min_val + 1);
                aax_param->SetType(AAX_eParameterType_Discrete);
                if (aax_param->Automatable()) {
                    AddSynchronizedParameter(*aax_param);
                }
                mParameterManager.AddParameter(aax_param.release());
            }
        }, param.semantics);
    }

    auto sample_rate = AAX_CSampleRate{};
    Controller()->GetSampleRate(&sample_rate);
    _kernel->reset(sample_rate, 2048); // How to get max frames?

    // Pro Tool Bypass
    // const auto bypass_id = AAX_CString{cDefaultMasterBypassID};
    // auto bypass_param = std::unique_ptr<AAX_IParameter>(new AAX_CParameter<bool>(
    //     bypass_id.CString(),
    //     AAX_CString{"Bypass (Pro Tools)"},
    //     false,
    //     AAX_CBinaryTaperDelegate<bool>(),
    //     AAX_CBinaryDisplayDelegate<bool>("Bypass", "On"),
    //     true
    // ));
    // bypass_param->AddShortenedName("Bypass");
    // bypass_param->SetNumberOfSteps(2);
    // bypass_param->SetType(AAX_eParameterType_Discrete);
    // mParameterManager.AddParameter(bypass_param.release());
    // mPacketDispatcher.RegisterPacket(bypass_id.CString(), AAX_FIELD_INDEX(Aax_context, bypass));

    return AAX_SUCCESS;
}
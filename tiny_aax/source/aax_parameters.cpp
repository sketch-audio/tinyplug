#include <cstring>

#include "aax_parameters.h"

#include "AAX_CBinaryTaperDelegate.h"
#include "AAX_CBinaryDisplayDelegate.h"
#include "AAX_CNumberDisplayDelegate.h"
#include "AAX_CStateTaperDelegate.h"
#include "AAX_CStateDisplayDelegate.h"
#include "AAX_CUnitDisplayDelegateDecorator.h"

#include "aax_adapters.h"
#include "aax_algorithm.h"

AAX_Result Aax_parameters::EffectInit()
{
    using namespace tiny;
    const auto tree = Param_model::build_tree();
    _specs = params::flatten_tree(tree);
    params::sort_param_specs_by_id(_specs);

    static const auto flat_map = params::flatten_tree(tree);
    static const auto identifiers = tiny::aax::flatten_tree_to_ids(tree);

    for (size_t i = 0; i < flat_map.size(); ++i) {
        const auto& param = flat_map[i];
        const auto& identifier = identifiers[i];

        using enum params::Units;
        switch (param.units) {
            case boolean: {
                auto aax_param = std::unique_ptr<AAX_IParameter>(new AAX_CParameter<bool>(
                    identifier.c_str(),
                    AAX_CString(param.name),
                    param.def_val > 0,
                    AAX_CBinaryTaperDelegate<bool>(),
                    AAX_CBinaryDisplayDelegate<bool>("No", "Yes"),
                    !param.hidden
                ));
                if (std::strlen(param.short_name) > 0) {
                    aax_param->AddShortenedName(param.short_name);
                }
                aax_param->SetNumberOfSteps(2);
                aax_param->SetType(AAX_eParameterType_Discrete);
                mParameterManager.AddParameter(aax_param.release());
                break;
            }
            case indexed: {
                if (!param.provides_labels()) break;

                const auto num_states = param.max_val - param.min_val + 1;

                auto aax_param = std::unique_ptr<AAX_IParameter>(new AAX_CParameter<uint32_t>(
                    identifier.c_str(),
                    AAX_CString(param.name),
                    param.def_val,
                    AAX_CStateTaperDelegate<uint32_t>(param.min_val, param.max_val),
                    AAX_CStateDisplayDelegate<uint32_t>(num_states, const_cast<const char**>(param.labels.data()), param.min_val),
                    !param.hidden
                ));
                if (std::strlen(param.short_name) > 0) {
                    aax_param->AddShortenedName(param.short_name);
                }
                aax_param->SetNumberOfSteps(num_states);
                aax_param->SetType(AAX_eParameterType_Discrete);
                mParameterManager.AddParameter(aax_param.release());
                break;
            }
            default: {
                using TaperDelegate = tiny::aax::ControlAdapterTaperDelegate<double, Param_model::Param_id>;
                using DisplayDelegate = AAX_CNumberDisplayDelegate<double, 1, 1>; // precision: 1, space after: 1
                const auto units_str = params::units_string(param.units);

                auto aax_param = std::unique_ptr<AAX_IParameter>(new AAX_CParameter<double>(
                    identifier.c_str(),
                    AAX_CString(param.name),
                    param.def_val,
                    TaperDelegate(param), // So we can use our own control adapter.
                    AAX_CUnitDisplayDelegateDecorator<double>(DisplayDelegate(), units_str.c_str()),
                    !param.hidden
                ));
                if (std::strlen(param.short_name) > 0) {
                    aax_param->AddShortenedName(param.short_name);
                }
                aax_param->SetNumberOfSteps(128); // This is apparently the default.
                aax_param->SetType(AAX_eParameterType_Continuous);
                mParameterManager.AddParameter(aax_param.release());
                break;
            }
        }
    }

    // Pro Tool Bypass
    const auto bypass_id = AAX_CString{cDefaultMasterBypassID};
    auto bypass_param = std::unique_ptr<AAX_IParameter>(new AAX_CParameter<bool>(
        bypass_id.CString(),
        AAX_CString{"Bypass (Pro Tools)"},
        false,
        AAX_CBinaryTaperDelegate<bool>(),
        AAX_CBinaryDisplayDelegate<bool>("Bypass", "On"),
        true
    ));
    bypass_param->AddShortenedName("Bypass");
    bypass_param->SetNumberOfSteps(2);
    bypass_param->SetType(AAX_eParameterType_Discrete);
    mParameterManager.AddParameter(bypass_param.release());
    mPacketDispatcher.RegisterPacket(bypass_id.CString(), AAX_FIELD_INDEX(Aax_context, bypass));

    return AAX_SUCCESS;
}
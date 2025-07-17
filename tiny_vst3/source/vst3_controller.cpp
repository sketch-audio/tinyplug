#include <cstring>

#include "pluginterfaces/base/ibstream.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

#include "user_plug.h"

#include "vst3_adapters.h"
#include "vst3_controller.h"

Steinberg::tresult PLUGIN_API Vst3_controller::initialize(Steinberg::FUnknown* context)
{
    // Here the plug-in will be instantiated.

    const auto result = Super::initialize(context);

    if (result != Steinberg::kResultOk)
        return result;

    using namespace tiny;
    const auto tree = Param_model::build_tree();
    _specs = params::flatten_tree(tree);
    params::sort_param_specs_by_id(_specs);

    // Here you could register some parameters.

    //static const auto tree = tiny::Param_model::build_tree();
    static const auto flat_map = params::flatten_tree(tree);
    static const auto [units, param_unit_ids] = tiny::vst3::flatten_tree_to_units(tree);
    
    for (const auto& unit : units) {
        auto unit_info = Steinberg::Vst::UnitInfo{
            .id = unit.unit_id,
            .parentUnitId = unit.parent_id,
            .programListId = Steinberg::Vst::kNoProgramListId
        };
        Steinberg::Vst::StringConvert::convert(unit.name, unit_info.name);
        addUnit(new Steinberg::Vst::Unit{unit_info});
    }

    auto resolve_flags = [](const Param_model::Spec& param) {
        auto result = int32_t{};

        if (param.hidden) {
            result |= Steinberg::Vst::ParameterInfo::kIsHidden;
        }
        else {
            result |= Steinberg::Vst::ParameterInfo::kCanAutomate;
        }

        if (param.provides_labels()) {
            result |= Steinberg::Vst::ParameterInfo::kIsList;
        }

        return result;
    };

    for (size_t i = 0; i < flat_map.size(); ++i) {
        const auto& param = flat_map[i];
        const auto& unit_id = param_unit_ids[i];

        const auto step_count = param.discrete ? static_cast<int32_t>(param.max_val - param.min_val) : 0;
        const auto& adapter = param.knob_adapter;
        //
        auto param_info = Steinberg::Vst::ParameterInfo{
            .id = static_cast<Steinberg::Vst::ParamID>(param.id),
            .stepCount = step_count,
            .defaultNormalizedValue = adapter.plain_to_norm(param, param.def_val),
            .unitId = unit_id.unit_id,
            .flags = resolve_flags(param)
        };

        // Shenanigans to get the name.
        Steinberg::Vst::StringConvert::convert(param.name, param_info.title);
        if (std::strlen(param.short_name) > 0) {
            Steinberg::Vst::StringConvert::convert(param.short_name, param_info.shortTitle);
        }
        Steinberg::Vst::StringConvert::convert(tiny::params::units_string(param.units), param_info.units);

        parameters.addParameter(param_info);
    }

    return result;
}

Steinberg::tresult PLUGIN_API Vst3_controller::terminate()
{
    // Here the Plug-in will be de-instantiated, last possibility to remove some memory!

    // Do not forget to call parent.
    return Super::terminate();
}

Steinberg::tresult PLUGIN_API Vst3_controller::setComponentState(Steinberg::IBStream* state)
{
	// Here you get the state of the component (processor part).
	if (!state)
		return Steinberg::kResultFalse;

	return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API Vst3_controller::setState(Steinberg::IBStream* /*state*/)
{
	// Here you get the state of the controller.

	return Steinberg::kResultTrue;
}

//------------------------------------------------------------------------
Steinberg::tresult PLUGIN_API Vst3_controller::getState(Steinberg::IBStream* /*state*/)
{
	// Here you are asked to deliver the state of the controller (if needed).
	// Note: the real state of your plug-in is saved in the processor.

	return Steinberg::kResultTrue;
}

//------------------------------------------------------------------------
Steinberg::tresult PLUGIN_API Vst3_controller::getParamStringByValue(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue valueNormalized, Steinberg::Vst::String128 string)
{
	// Called by host to get a string for given normalized value of a specific parameter.
    // (without having to set the value!)
    if (tag >= tiny::Param_model::num_params) return Steinberg::kResultFalse;

    using namespace tiny;
    const auto& param = _specs[tag];
    const auto& adapter = param.knob_adapter;
    const auto plain = adapter.norm_to_plain(param, valueNormalized);
    const auto str = Param_model::format_string(plain, param, _uivalues, false); // Don't include units.
    Steinberg::Vst::StringConvert::convert(str, string);

    return Steinberg::kResultTrue;
}

//------------------------------------------------------------------------
Steinberg::tresult PLUGIN_API Vst3_controller::getParamValueByString(Steinberg::Vst::ParamID tag, Steinberg::Vst::TChar* string, Steinberg::Vst::ParamValue& valueNormalized)
{
	// Called by host to get a normalized value from a string representation of a specific parameter.
    // (without having to set the value!)
    if (tag >= tiny::Param_model::num_params) return Steinberg::kResultFalse;

    using namespace tiny;
    const auto& param = _specs[tag];
    const auto& adapter = param.knob_adapter;
    const auto str = Steinberg::Vst::StringConvert::convert(string);
    const auto value = Param_model::format_value(str, param);
    valueNormalized = adapter.plain_to_norm(param, value);

    return Steinberg::kResultTrue;
}

Steinberg::Vst::ParamValue PLUGIN_API Vst3_controller::normalizedParamToPlain(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue valueNormalized)
{
    const auto& param = _specs[tag];
    const auto& adapter = param.knob_adapter;
    return adapter.norm_to_plain(param, valueNormalized);
}

Steinberg::Vst::ParamValue PLUGIN_API Vst3_controller::plainParamToNormalized(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue plainValue)
{
    const auto& param = _specs[tag];
    const auto& adapter = param.knob_adapter;
    return adapter.plain_to_norm(param, plainValue);
}

Steinberg::Vst::ParamValue PLUGIN_API Vst3_controller::getParamNormalized(Steinberg::Vst::ParamID tag)
{
    return Super::getParamNormalized(tag);
}

Steinberg::tresult PLUGIN_API Vst3_controller::setParamNormalized(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue value)
{
	const auto result = Super::setParamNormalized(tag, value);
	return result;
}

Steinberg::tresult PLUGIN_API Vst3_controller::setComponentHandler(Steinberg::Vst::IComponentHandler* handler)
{
    const auto result = Super::setComponentHandler(handler);
    return result;
}

//------------------------------------------------------------------------
Steinberg::IPlugView* PLUGIN_API Vst3_controller::createView(Steinberg::FIDString name)
{
	// Here the Host wants to open your editor (if you have one).
	if (Steinberg::FIDStringsEqual(name, Steinberg::Vst::ViewType::kEditor))
	{
        // Create your editor here and return a IPlugView ptr of it.
        view = new Vst3_view();
        return view;
    }
    
    return nullptr;
}
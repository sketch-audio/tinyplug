#include <cstring>

#include "pluginterfaces/base/ibstream.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

#include "tinyplug/tinyplug.h"

#include "param_model.h"

#include "vst3_adapters.h"
#include "vst3_controller.h"

namespace tiny {

Steinberg::tresult PLUGIN_API Vst3_controller::initialize(Steinberg::FUnknown* context)
{
    // Here the plug-in will be instantiated.

    const auto result = Super::initialize(context);

    if (result != Steinberg::kResultOk)
        return result;

    // Here you could register some parameters.

    const auto& tree = _params.tree();
    const auto& params = _params.presentation_specs();
    const auto [units, param_unit_ids] = tree_to_units(tree);

    for (const auto& unit : units) {
        auto unit_info = Steinberg::Vst::UnitInfo{
            .id = unit.unit_id,
            .parentUnitId = unit.parent_id,
            .programListId = Steinberg::Vst::kNoProgramListId
        };
        Steinberg::Vst::StringConvert::convert(unit.name, unit_info.name);
        addUnit(new Steinberg::Vst::Unit{unit_info});
    }

    for (size_t i = 0; i < params.size(); ++i) {
        const auto& param = params[i];
        const auto& unit_id = param_unit_ids[i];

        auto param_info = std::visit(Inline_visitor{
            [&](const Bool_semantics& b) {
                return Steinberg::Vst::ParameterInfo{
                    .id = static_cast<Steinberg::Vst::ParamID>(param.id),
                    .stepCount = 1,
                    .defaultNormalizedValue = b.knob_adapter.plain_to_norm(b, b.def_val),
                    .unitId = unit_id.unit_id,
                    .flags = {}
                };
            },
            [&](const List_semantics& l) {
                return Steinberg::Vst::ParameterInfo{
                    .id = static_cast<Steinberg::Vst::ParamID>(param.id),
                    .stepCount = static_cast<int32_t>(l.labels.size() - 1),
                    .defaultNormalizedValue = l.knob_adapter.plain_to_norm(l, l.def_val),
                    .unitId = unit_id.unit_id,
                    .flags = Steinberg::Vst::ParameterInfo::kIsList
                };
            },
            [&](const Float_semantics& f) {
                return Steinberg::Vst::ParameterInfo{
                    .id = static_cast<Steinberg::Vst::ParamID>(param.id),
                    .stepCount = 0,
                    .defaultNormalizedValue = f.knob_adapter.plain_to_norm(f, f.def_val),
                    .unitId = unit_id.unit_id,
                    .flags = {}
                };
            },
            [&](const Int_semantics& i) {
                return Steinberg::Vst::ParameterInfo{
                    .id = static_cast<Steinberg::Vst::ParamID>(param.id),
                    .stepCount = i.max_val - i.min_val,
                    .defaultNormalizedValue = i.knob_adapter.plain_to_norm(i, i.def_val),
                    .unitId = unit_id.unit_id,
                    .flags = {}
                };
            }
        }, param.semantics);

        // Flags
        param_info.flags |= (param.hidden ? Steinberg::Vst::ParameterInfo::kIsHidden : Steinberg::Vst::ParameterInfo::kCanAutomate);

        // Shenanigans to get the name.
        Steinberg::Vst::StringConvert::convert(param.name, param_info.title);
        if (std::strlen(param.short_name) > 0) {
            Steinberg::Vst::StringConvert::convert(param.short_name, param_info.shortTitle);
        }

        parameters.addParameter(param_info);
    }

    const auto num_exports = enum_raw(Param_model::Export_id::num_exports);

    for (auto i = decltype(num_exports){}; i < num_exports; ++i) {
        auto export_info = Steinberg::Vst::ParameterInfo{
            .id = static_cast<Steinberg::Vst::ParamID>(i + export_param_offset),
            .title = u"",
            .shortTitle = u"",
            .stepCount = 0,
            .defaultNormalizedValue = 0,
            .unitId = Steinberg::Vst::kRootUnitId,
            .flags = (Steinberg::Vst::ParameterInfo::kIsReadOnly | Steinberg::Vst::ParameterInfo::kIsHidden)
        };
        parameters.addParameter(export_info);
    }

    // Add the latency parameter.
    auto latency_info = Steinberg::Vst::ParameterInfo{
        .id = latency_param_id,
        .title = u"Latency",
        .shortTitle = u"Latency",
        .stepCount = 0,
        .defaultNormalizedValue = 0,
        .unitId = Steinberg::Vst::kRootUnitId,
        .flags = (Steinberg::Vst::ParameterInfo::kIsReadOnly | Steinberg::Vst::ParameterInfo::kIsHidden)
    };
    parameters.addParameter(latency_info);

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
    if (tag >= User_params::num_params) return Steinberg::kResultFalse;

    const auto& params = _params.kernel_specs();
    const auto& param = params[tag];
    const auto host = Value_conv::knob_to_host(valueNormalized, param.semantics);
    const auto str = Host_formatter::format_string(host, param.semantics);
    Steinberg::Vst::StringConvert::convert(str, string);

    return Steinberg::kResultTrue;
}

//------------------------------------------------------------------------
Steinberg::tresult PLUGIN_API Vst3_controller::getParamValueByString(Steinberg::Vst::ParamID tag, Steinberg::Vst::TChar* string, Steinberg::Vst::ParamValue& valueNormalized)
{
    // Called by host to get a normalized value from a string representation of a specific parameter.
    // (without having to set the value!)
    if (tag >= User_params::num_params) return Steinberg::kResultFalse;

    const auto& params = _params.kernel_specs();
    const auto& param = params[tag];
    const auto str = Steinberg::Vst::StringConvert::convert(string);
    if (const auto plain = Host_formatter::format_value(str, param.semantics)) {
        valueNormalized = Value_conv::plain_to_knob(*plain, param.semantics);
        return Steinberg::kResultTrue;
    }

    return Steinberg::kResultFalse;
}

Steinberg::Vst::ParamValue PLUGIN_API Vst3_controller::normalizedParamToPlain(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue valueNormalized)
{
    const auto& params = _params.kernel_specs();
    const auto& param = params[tag];
    return Value_conv::knob_to_plain(valueNormalized, param.semantics);
}

Steinberg::Vst::ParamValue PLUGIN_API Vst3_controller::plainParamToNormalized(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue plainValue)
{
    const auto& params = _params.kernel_specs();
    const auto& param = params[tag];
    return Value_conv::plain_to_knob(plainValue, param.semantics);
}

Steinberg::Vst::ParamValue PLUGIN_API Vst3_controller::getParamNormalized(Steinberg::Vst::ParamID tag)
{
    return Super::getParamNormalized(tag);
}

Steinberg::tresult PLUGIN_API Vst3_controller::setParamNormalized(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue value)
{
    const auto result = Super::setParamNormalized(tag, value);

    // Do not forward setParam to UI during gestures.
    if (_gestured.find(tag) != _gestured.end()) { return result; }

    // Is it a parameter?
    if (tag < num_params) {
        _oqueue.push(Set_param{.id = tag, .value = value});
    }
    // Is it an export?
    else if (tag >= export_param_offset && tag < export_param_offset + num_exports) {
        const auto id = tag - export_param_offset;
        _oqueue.push(Set_export{.id = id, .value = value});
        _last_exports[id] = value;
    }
    // Is it a latency change?
    else if (tag == latency_param_id) {
        if (auto* handler = getComponentHandler()) {
            handler->restartComponent(Steinberg::Vst::kLatencyChanged);
        }
    }

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
        // A workaround for now, push all exports into the queue.
        // This is so we can get correct values on first appearance.
        enumerate<uint32_t>(_last_exports, [this](auto i, const auto& e) {
            _oqueue.push(Set_export{.id = i, .value = e});
        });

        view = new Vst3_view({
            .get_knob_value = [this](auto id) { return getParamNormalized(id); },
            .pop_event = [this](auto& e) { return _oqueue.pop(e); },
            .action_handler = [this](auto& a) {
                std::visit(Inline_visitor{
                    [this](const Action_start& s) { beginEdit(s.id); _gestured.insert(s.id); },
                    [this](const Set_param& s) {
                        if (setParamNormalized(s.id, s.value) == Steinberg::kResultTrue) {
                            performEdit(s.id, getParamNormalized(s.id));
                        }
                    },
                    [this](const Action_end& s) { endEdit(s.id); _gestured.erase(s.id); },
                }, a);
            }
        });
        return view;
    }

    return nullptr;
}

} // namespace tiny
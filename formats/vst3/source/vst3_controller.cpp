#include <cstring>

#include "pluginterfaces/base/ibstream.h"
#include "public.sdk/source/vst/utility/stringconvert.h"
#include "base/source/fstreamer.h"

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

    const auto& params = _param_infos.presentation_specs();
    const auto [unit_infos, param_unit_ids] = tree_to_units(_param_infos.tree());

    for (const auto& unit : unit_infos) {
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
            [&](const Bool_semantics&) {
                return Steinberg::Vst::ParameterInfo{
                    .id = static_cast<Steinberg::Vst::ParamID>(param.id),
                    .stepCount = 1,
                    .defaultNormalizedValue = get_knob_default(param),
                    .unitId = unit_id.unit_id,
                    .flags = {}
                };
            },
            [&](const List_semantics& l) {
                return Steinberg::Vst::ParameterInfo{
                    .id = static_cast<Steinberg::Vst::ParamID>(param.id),
                    .stepCount = static_cast<int32_t>(l.items.size() - 1),
                    .defaultNormalizedValue = get_knob_default(param),
                    .unitId = unit_id.unit_id,
                    .flags = Steinberg::Vst::ParameterInfo::kIsList
                };
            },
            [&](const Int_semantics& i) {
                return Steinberg::Vst::ParameterInfo{
                    .id = static_cast<Steinberg::Vst::ParamID>(param.id),
                    .stepCount = i.max_val - i.min_val,
                    .defaultNormalizedValue = get_knob_default(param),
                    .unitId = unit_id.unit_id,
                    .flags = {}
                };
            },
            [&](const Real_semantics&) {
                return Steinberg::Vst::ParameterInfo{
                    .id = static_cast<Steinberg::Vst::ParamID>(param.id),
                    .stepCount = 0,
                    .defaultNormalizedValue = get_knob_default(param),
                    .unitId = unit_id.unit_id,
                    .flags = {}
                };
            },
        }, param.semantics);

        // Resolve flags for policy. 
        param_info.flags |= [policy = param.policy]() {
            using enum Host_policy;
            using Vst3_flags = Steinberg::Vst::ParameterInfo::ParameterFlags;
            switch (policy) {
                case automation: return Vst3_flags::kCanAutomate;
                case control: return Vst3_flags::kNoFlags; // Will any hosts display a control?
                case state: return Vst3_flags{Vst3_flags::kIsHidden | Vst3_flags::kIsReadOnly};
                case interface: return Vst3_flags{Vst3_flags::kIsHidden | Vst3_flags::kIsReadOnly};
                default: return Vst3_flags::kNoFlags;
            }
        }();

        // Shenanigans to get the name.
        Steinberg::Vst::StringConvert::convert(param.name, param_info.title);
        if (std::strlen(param.short_name) > 0) {
            Steinberg::Vst::StringConvert::convert(param.short_name, param_info.shortTitle);
        }

        parameters.addParameter(param_info);
    }

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

    // Streamer convenience wrapper. 
    auto streamer = Steinberg::IBStreamer{state};

    auto header = State_header{};
    if (!streamer.readInt32uArray(header.data(), static_cast<int32_t>(header.size()))) {
        return Steinberg::kResultFalse;
    }

    // Validate header.
    assert(header[0] == Plug_info::framework_code && "Unexpected framework code.");
    assert(header[1] == Plug_info::manufacturer_code && "Unexpected manufacturer code.");
    assert(header[2] == Plug_info::plugin_code && "Unexpected plug-in code.");
    assert(header[3] > 0 && "Unexpected tree version.");

    const auto tree_version = max_tree_version(_param_infos.tree());
    const auto state_version = header[3];

    // Notify view (if not an interface parameter).
    auto do_notify = [this](auto& param, auto knob_value) {
        if (param.policy != Host_policy::interface) {
            setParamNormalized(param.id, knob_value);
        }
    };

    if (tree_version <= state_version) {
        // Implies "num params in tree" <= "num params in state"
        // We should be able to safely read `num_params` floats.
        for (auto i = decltype(num_params){}; i < num_params; ++i) {
            auto knob_value = float{};
            if (!streamer.readFloat(knob_value)) {
                return Steinberg::kResultFalse;
            }

            // Send knob value to view.
            const auto& param = _param_infos.param_for(i);
            do_notify(param, knob_value);
        }
    }
    else if (tree_version > state_version) {
        // Implies "num params in tree" > "num params in state"
        const auto num_state = num_params_with_version(_param_infos.tree(), state_version);

        // Set values stored in state.
        for (auto i = decltype(num_state){}; i < num_state; ++i) {
            auto knob_value = float{};
            if (!streamer.readFloat(knob_value)) {
                return Steinberg::kResultFalse;
            }

            // Send knob value to view.
            const auto& param = _param_infos.param_for(i);
            do_notify(param, knob_value);
        }

        // Set remaining parameters to defaults. 
        for (auto i = num_state; i < num_params; ++i) {
            const auto& param = _param_infos.param_for(i);
            const auto knob_value = get_knob_default(param);
            do_notify(param, knob_value);
        }
    }

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

    const auto& params = _param_infos.kernel_specs();
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

    const auto& param = _param_infos.param_for(tag);
    const auto str = Steinberg::Vst::StringConvert::convert(string);
    if (const auto plain = Host_formatter::format_value(str, param.semantics)) {
        valueNormalized = Value_conv::plain_to_knob(*plain, param.semantics);
        return Steinberg::kResultTrue;
    }

    return Steinberg::kResultFalse;
}

Steinberg::Vst::ParamValue PLUGIN_API Vst3_controller::normalizedParamToPlain(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue valueNormalized)
{
    const auto& param = _param_infos.param_for(tag);
    return Value_conv::knob_to_plain(valueNormalized, param.semantics);
}

Steinberg::Vst::ParamValue PLUGIN_API Vst3_controller::plainParamToNormalized(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue plainValue)
{
    const auto& param = _param_infos.param_for(tag);
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
        // Make the UI connection.
        auto receiver = Ui_receiver{
            .get_knob_value = [this](auto id) {
                return getParamNormalized(id);
            },
            .pop_event = [this](auto& e) {
                return _oqueue.pop(e);
            },
            .action_handler = [this](auto& a) {
                std::visit(Inline_visitor{
                    [this](const Action_start& s) {
                        beginEdit(s.id);
                        _gestured.insert(s.id);
                    },
                    [this](const Set_param& s) {
                        if (setParamNormalized(s.id, s.value) == Steinberg::kResultTrue) {
                            performEdit(s.id, getParamNormalized(s.id));
                        }
                    },
                    [this](const Action_end& s) {
                        endEdit(s.id);
                        _gestured.erase(s.id);
                    },
            }, a);
            }
        };

        // A workaround for now, push all exports into the queue.
        // This is so we can get correct values on first appearance.
        enumerate<uint32_t>(_last_exports, [this](auto i, const auto& e) {
            _oqueue.push(Set_export{.id = i, .value = e});
        });

        return new Vst3_view(receiver, _editor, this);
    }

    return nullptr;
}

} // namespace tiny
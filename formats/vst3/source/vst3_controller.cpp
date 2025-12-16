#include <cstring>

#include "pluginterfaces/base/ibstream.h"
#include "public.sdk/source/vst/utility/stringconvert.h"
#include "base/source/fstreamer.h"

#include "tinyplug/tinyplug.h"

#include "models/meter_model.h"
#include "models/param_model.h"

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

    const auto& params = User_params::param_specs(Param_order::Presentation);
    const auto [unit_infos, param_unit_ids] = tree_to_units(User_params::param_tree());

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
                    .id = static_cast<Steinberg::Vst::ParamID>(param.address),
                    .stepCount = 1,
                    .defaultNormalizedValue = get_knob_default(param),
                    .unitId = unit_id.unit_id,
                    .flags = {}
                };
            },
            [&](const List_semantics& l) {
                return Steinberg::Vst::ParameterInfo{
                    .id = static_cast<Steinberg::Vst::ParamID>(param.address),
                    .stepCount = static_cast<int32_t>(l.items.size() - 1),
                    .defaultNormalizedValue = get_knob_default(param),
                    .unitId = unit_id.unit_id,
                    .flags = Steinberg::Vst::ParameterInfo::kIsList
                };
            },
            [&](const Int_semantics& i) {
                return Steinberg::Vst::ParameterInfo{
                    .id = static_cast<Steinberg::Vst::ParamID>(param.address),
                    .stepCount = i.max_val - i.min_val,
                    .defaultNormalizedValue = get_knob_default(param),
                    .unitId = unit_id.unit_id,
                    .flags = {}
                };
            },
            [&](const Fixed_semantics&) {
                return Steinberg::Vst::ParameterInfo{
                    .id = static_cast<Steinberg::Vst::ParamID>(param.address),
                    .stepCount = 0,
                    .defaultNormalizedValue = get_knob_default(param),
                    .unitId = unit_id.unit_id,
                    .flags = {}
                };
            },
            [&](const Real_semantics&) {
                return Steinberg::Vst::ParameterInfo{
                    .id = static_cast<Steinberg::Vst::ParamID>(param.address),
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
                case hidden: return Vst3_flags{Vst3_flags::kIsHidden | Vst3_flags::kIsReadOnly};
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

    for (auto i = decltype(num_meters){}; i < num_meters; ++i) {
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

// MARK: - processor state

Steinberg::tresult PLUGIN_API Vst3_controller::setComponentState(Steinberg::IBStream* state)
{
    // Here you get the state of the component (processor part).
    if (!state) {
        return Steinberg::kResultFalse;
    }

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

    const auto num_state_params = header[3];

    // Notify view (if not an interface parameter).
    auto do_notify = [this](auto& param, auto knob_value) {
        if (param.policy != Host_policy::interface) {
            setParamNormalized(param.address, knob_value);
        }
    };

    // Read processor state into temporary vector.
    auto state_params = std::vector<float>(num_state_params);
    for (auto i = decltype(num_state_params){}; i < num_state_params; ++i) {
        if (!streamer.readFloat(state_params[i])) {
            return Steinberg::kResultFalse;
        }
    }

    if (num_params <= num_state_params) {
        // Set values stored in state.
        for (auto i = decltype(num_params){}; i < num_params; ++i) {
            const auto knob_value = state_params[i];
            const auto& param = User_params::param_spec(i);
            do_notify(param, knob_value);
        }
    }
    else {
        // Set values stored in state.
        for (auto i = decltype(num_state_params){}; i < num_state_params; ++i) {
            const auto knob_value = state_params[i];
            const auto& param = User_params::param_spec(i);
            do_notify(param, knob_value);
        }

        // Set remaining parameters to defaults.
        for (auto i = num_state_params; i < num_params; ++i) {
            const auto& param = User_params::param_spec(i);
            const auto knob_value = get_knob_default(param);
            do_notify(param, knob_value);
        }
    }

    return Steinberg::kResultOk;
}

// MARK: - editor state

Steinberg::tresult PLUGIN_API Vst3_controller::setState(Steinberg::IBStream* state)
{
    // Here you get the state of the controller.
    if (!state) {
        return Steinberg::kResultFalse;
    }

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

    const auto num_kv_pairs = header[3];

    // Helper
    auto read_container = [&](auto& container) {
        auto num = uint32_t{};
        if (!streamer.readInt32u(num)) {
            return false;
        }
        container.resize(num);
        if (num > 0) {
            if (!streamer.readRaw(container.data(), sizeof(container[0]) * num)) {
                return false;
            }
        }
        return true;
    };

    // Read editor state.
    auto edit_state = State_map{};
    for (auto i = decltype(num_kv_pairs){}; i < num_kv_pairs; ++i) {
        // Read key.
        auto key = std::string{};
        if (!read_container(key)) {
            return Steinberg::kResultFalse;
        }

        // Read the type tag.
        auto tag_raw = uint32_t{};
        if (!streamer.readInt32u(tag_raw)) {
            return Steinberg::kResultFalse;
        }
        const auto tag = static_cast<State_tag>(tag_raw);

        // Read the value according to the tag.
        auto value = State_item{};
        switch (tag) {
            case State_tag::bool_: {
                auto v = bool{};
                if (streamer.readBool(v)) {
                    value = v;
                    break;
                }
                return Steinberg::kResultFalse;
            }
            case State_tag::int_: {
                auto v = int32_t{};
                if (streamer.readInt32(v)) {
                    value = v;
                    break;
                }
                return Steinberg::kResultFalse;
            }
            case State_tag::double_: {
                auto v = double{};
                if (streamer.readDouble(v)) {
                    value = v;
                    break;
                }
                return Steinberg::kResultFalse;
            }
            case State_tag::string_: {
                auto v = std::string{};
                if (read_container(v)) {
                    value = std::move(v);
                    break;
                }
                return Steinberg::kResultFalse;
            }
            case State_tag::bytes_: {
                auto v = std::vector<uint8_t>{};
                if (read_container(v)) {
                    value = std::move(v);
                    break;
                }
                return Steinberg::kResultFalse;
            }
            default:
                assert(false && "Unknown editor state type.");
                return Steinberg::kResultFalse;
        }

        edit_state.emplace(std::move(key), std::move(value));
    }

    _editor->load_state(edit_state);

    return Steinberg::kResultTrue;
}

//------------------------------------------------------------------------
Steinberg::tresult PLUGIN_API Vst3_controller::getState(Steinberg::IBStream* state)
{
    // Here you are asked to deliver the state of the controller (if needed).
    // Note: the real state of your plug-in is saved in the processor.
    if (!state) {
        return Steinberg::kResultFalse;
    }

    // Streamer convenience wrapper.
    auto streamer = Steinberg::IBStreamer{state};

    const auto edit_state = _editor->save_state();
    const auto num_editor_items = static_cast<uint32_t>(edit_state.size());

    const auto header = State_header{
        Plug_info::framework_code, // Reserved
        Plug_info::manufacturer_code,
        Plug_info::plugin_code,
        num_editor_items
    };

    if (!streamer.writeInt32uArray(header.data(), static_cast<int32_t>(header.size()))) {
        return Steinberg::kResultFalse;
    }

    // Helper
    auto write_container = [&](const auto& container) {
        const auto num = static_cast<uint32_t>(container.size());
        if (!streamer.writeInt32u(num)) {
            return false;
        }
        if (num > 0) {
            if (!streamer.writeRaw(container.data(), sizeof(container[0]) * num)) {
                return false;
            }
        }
        return true;
    };

    // Write editor state.
    for (const auto& [key, val] : edit_state) {
        // Write key.
        if (!write_container(key)) {
            return Steinberg::kResultFalse;
        }

        // Write the type tag.
        const auto tag = tag_for(val);
        if (!streamer.writeInt32u(enum_raw(tag))) {
            return Steinberg::kResultFalse;
        }

        // Write the value according to the tag.
        switch (tag) {
            case State_tag::bool_: {
               const auto value = std::get_if<bool>(&val);
               if (value && streamer.writeBool(*value)) {
                   break;
               }
               return Steinberg::kResultFalse;
            }
            case State_tag::int_: {
                const auto value = std::get_if<int32_t>(&val);
                if (value && streamer.writeInt32(*value)) {
                    break;
                }
                return Steinberg::kResultFalse;
            }
            case State_tag::double_: {
                const auto value = std::get_if<double>(&val);
                if (value && streamer.writeDouble(*value)) {
                    break;
                }
                return Steinberg::kResultFalse;
            }
            case State_tag::string_: {
                const auto value = std::get_if<std::string>(&val);
                if (value && write_container(*value)) {
                    break;
                }
                return Steinberg::kResultFalse;
            }
            case State_tag::bytes_: {
                const auto value = std::get_if<std::vector<uint8_t>>(&val);
                if (value && write_container(*value)) {
                    break;
                }
                return Steinberg::kResultFalse;
            }
            default:
                assert(false && "Unknown editor state type.");
                return Steinberg::kResultFalse;
        }
    }

    return Steinberg::kResultTrue;
}

//------------------------------------------------------------------------
Steinberg::tresult PLUGIN_API Vst3_controller::getParamStringByValue(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue valueNormalized, Steinberg::Vst::String128 string)
{
    // Called by host to get a string for given normalized value of a specific parameter.
    // (without having to set the value!)
    if (tag >= User_params::num_params) return Steinberg::kResultFalse;

    const auto& params = User_params::param_specs(Param_order::Indexable);
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

    const auto& param = User_params::param_spec(tag);
    const auto str = Steinberg::Vst::StringConvert::convert(string);
    if (const auto plain = Host_formatter::format_value(str, param.semantics)) {
        valueNormalized = Value_conv::plain_to_knob(*plain, param.semantics);
        return Steinberg::kResultTrue;
    }

    return Steinberg::kResultFalse;
}

Steinberg::Vst::ParamValue PLUGIN_API Vst3_controller::normalizedParamToPlain(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue valueNormalized)
{
    const auto& param = User_params::param_spec(tag);
    return Value_conv::knob_to_plain(valueNormalized, param.semantics);
}

Steinberg::Vst::ParamValue PLUGIN_API Vst3_controller::plainParamToNormalized(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue plainValue)
{
    const auto& param = User_params::param_spec(tag);
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
        _to_editor.push(Set_param{.address = tag, .value = value});
    }
    // Is it an export?
    else if (tag >= export_param_offset && tag < export_param_offset + num_meters) {
        const auto id = tag - export_param_offset;

        // Convert back to plain for UI.
        const auto& spec = User_meters::meter_spec(id);
        const auto plain = norm_to_plain(value, spec.range);
        _to_editor.push(Set_meter{.address = id, .value = plain});

        _last_meters[id] = plain; // Cache plain because we might dump to UI.
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
                return _to_editor.pop(e);
            },
            .action_handler = [this](auto& a) {
                std::visit(Inline_visitor{
                    [this](const Action_start& s) {
                        beginEdit(s.address);
                        _gestured.insert(s.address);
                    },
                    [this](const Set_param& s) {
                        if (setParamNormalized(s.address, s.value) == Steinberg::kResultTrue) {
                            performEdit(s.address, getParamNormalized(s.address));
                        }
                    },
                    [this](const Action_end& s) {
                        endEdit(s.address);
                        _gestured.erase(s.address);
                    },
            }, a);
            }
        };

        // A workaround for now, push all exports into the queue.
        // This is so we can get correct values on first appearance.
        enumerate<uint32_t>(_last_meters, [this](auto i, const auto& e) {
            _to_editor.push(Set_meter{.address = i, .value = e});
        });

        return new Vst3_view{{.controller = this, .editor = &(*_editor), .receiver = std::move(receiver), .tasks = &_tasks}};
    }

    return nullptr;
}

} // namespace tiny
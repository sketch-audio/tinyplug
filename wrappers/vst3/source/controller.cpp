#include <algorithm>
#include <cstring>

#include "pluginterfaces/base/ibstream.h"
#include "public.sdk/source/vst/utility/stringconvert.h"
#include "base/source/fstreamer.h"

#include "tinyplug/tinyplug.hpp"

#include "models/meters.hpp"
#include "models/params.hpp"

#include "adapters.hpp"
#include "controller.hpp"
#include "messaging.hpp"

namespace tiny::vst3 {

#if TINY_HAS_WORKER

constexpr auto k_worker_from_processor_id = "tiny/worker/from_processor";
constexpr auto k_worker_to_processor_id   = "tiny/worker/to_processor";

auto Controller::_setup_worker() -> void
{
    // Processor → worker: decode incoming IMessages (sent by the
    // processor-side shuttle thread) and push into the from-processor
    // inbound queue. The worker thread drains and dispatches.
    _router.register_handler(k_worker_from_processor_id, [this](std::span<const std::byte> bytes, uint32_t tag) {
        using From_proc = typename User_worker::Model::From_processor;
        _worker_from_proc.push(vst3::reconstruct_variant<From_proc>(bytes, tag));
    });

    // Editor → worker: direct in-process push.
    try_bind_worker(*_editor, Worker_editor_actor{
        [this](const auto& m) { return _worker_from_edit.push(m); }
    });

    // Worker → processor: install a post-cycle on the runner that drains
    // _worker_to_proc and forwards each reply via IMessage. Runs on the
    // worker thread (non-realtime), so allocation is fine.
    _worker_runner.set_post_cycle([this]() {
        if constexpr (!std::is_same_v<typename User_worker::Model::To_processor, std::monostate>) {
            auto reply = typename User_worker::Model::To_processor{};
            while (_worker_to_proc.pop(reply)) {
                _to_proc.send_variant(k_worker_to_processor_id, reply);
            }
        }
    });
}

Steinberg::tresult PLUGIN_API Controller::notify(Steinberg::Vst::IMessage* message)
{
    if (_router.dispatch(message)) return Steinberg::kResultOk;
    return Super::notify(message);
}

#endif // TINY_HAS_WORKER

auto Controller::_drain_worker_to_editor() -> void
{
#if TINY_HAS_WORKER
    try_drain_worker_to_editor(*_editor, _worker_to_edit);
#endif
}


Steinberg::tresult PLUGIN_API Controller::initialize(Steinberg::FUnknown* context)
{
    using namespace params;
    // Here the plug-in will be instantiated.

    const auto result = Super::initialize(context);

    if (result != Steinberg::kResultOk)
        return result;

#if TINY_HAS_WORKER
    _worker_runner.start(0); // Sample rate unknown to controller; plug-in author
                             // can push it via a From_processor message if needed.
#endif

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
            [&](const params::Semantics::Bool&) {
                return Steinberg::Vst::ParameterInfo{
                    .id = static_cast<Steinberg::Vst::ParamID>(param.identity.address),
                    .stepCount = 1,
                    .defaultNormalizedValue = Value_helper::default_value(param, Space::Knob),
                    .unitId = unit_id.unit_id,
                    .flags = {}
                };
            },
            [&](const params::Semantics::List& l) {
                return Steinberg::Vst::ParameterInfo{
                    .id = static_cast<Steinberg::Vst::ParamID>(param.identity.address),
                    .stepCount = static_cast<int32_t>(l.items.size() - 1),
                    .defaultNormalizedValue = Value_helper::default_value(param, Space::Knob),
                    .unitId = unit_id.unit_id,
                    .flags = Steinberg::Vst::ParameterInfo::kIsList
                };
            },
            [&](const params::Semantics::Int& i) {
                return Steinberg::Vst::ParameterInfo{
                    .id = static_cast<Steinberg::Vst::ParamID>(param.identity.address),
                    .stepCount = i.max_val - i.min_val,
                    .defaultNormalizedValue = Value_helper::default_value(param, Space::Knob),
                    .unitId = unit_id.unit_id,
                    .flags = {}
                };
            },
            [&](const params::Semantics::Fixed&) {
                return Steinberg::Vst::ParameterInfo{
                    .id = static_cast<Steinberg::Vst::ParamID>(param.identity.address),
                    .stepCount = 0,
                    .defaultNormalizedValue = Value_helper::default_value(param, Space::Knob),
                    .unitId = unit_id.unit_id,
                    .flags = {}
                };
            },
            [&](const params::Semantics::Real&) {
                return Steinberg::Vst::ParameterInfo{
                    .id = static_cast<Steinberg::Vst::ParamID>(param.identity.address),
                    .stepCount = 0,
                    .defaultNormalizedValue = Value_helper::default_value(param, Space::Knob),
                    .unitId = unit_id.unit_id,
                    .flags = {}
                };
            },
        }, param.semantics);

        // Resolve flags for policy. 
        param_info.flags |= [policy = param.policy]() {
            using enum params::Policy;
            using Vst3_flags = Steinberg::Vst::ParameterInfo::ParameterFlags;
            switch (policy) {
                case Automation: return Vst3_flags::kCanAutomate;
                case Control: return Vst3_flags::kNoFlags; // Will any hosts display a control?
                // case Hidden: return Vst3_flags{Vst3_flags::kIsHidden | Vst3_flags::kIsReadOnly}; // Studio Pro doesn't send editor changes to the processor for hidden/read-only combo.
                // case Interface: return Vst3_flags{Vst3_flags::kIsHidden | Vst3_flags::kIsReadOnly};
                default: return Vst3_flags::kNoFlags;
            }
        }();

        // Shenanigans to get the name.
        Steinberg::Vst::StringConvert::convert(param.name, param_info.title);
        if (!param.short_name.empty()) {
            Steinberg::Vst::StringConvert::convert(param.short_name, param_info.shortTitle);
        }

        parameters.addParameter(param_info);
    }

    for (auto i = decltype(num_meters){}; i < num_meters; ++i) {
        auto export_info = Steinberg::Vst::ParameterInfo{
            .id = static_cast<Steinberg::Vst::ParamID>(i + export_param_offset),
            .title = u"Meter",
            .shortTitle = u"Meter",
            .stepCount = 0,
            .defaultNormalizedValue = 0,
            .unitId = Steinberg::Vst::kRootUnitId,
            .flags = Steinberg::Vst::ParameterInfo::kIsReadOnly // This is now consistent with the Steinberg example.
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
        .flags = Steinberg::Vst::ParameterInfo::kIsReadOnly
    };
    parameters.addParameter(latency_info);

    // Add the bypass parameter.
    auto bypass_info = Steinberg::Vst::ParameterInfo{
        .id = bypass_param_id,
        .title = u"Bypass",
        .shortTitle = u"Bypass",
        .stepCount = 1,
        .defaultNormalizedValue = 0,
        .unitId = Steinberg::Vst::kRootUnitId,
        .flags = (Steinberg::Vst::ParameterInfo::kCanAutomate | Steinberg::Vst::ParameterInfo::kIsBypass)
    };
    parameters.addParameter(bypass_info);

    return result;
}

Steinberg::tresult PLUGIN_API Controller::terminate()
{
    // Here the Plug-in will be de-instantiated, last possibility to remove some memory!

    // Do not forget to call parent.
    return Super::terminate();
}

// MARK: - processor state

Steinberg::tresult PLUGIN_API Controller::setComponentState(Steinberg::IBStream* state)
{
    using namespace params;

    // Here you get the state of the component (processor part).
    if (!state) {
        return Steinberg::kResultFalse;
    }

    // Streamer convenience wrapper. 
    auto streamer = Steinberg::IBStreamer{state};

    auto header = State_rules::Vst3::Header{};
    if (!streamer.readInt32uArray(header.data(), static_cast<int32_t>(header.size()))) {
        return Steinberg::kResultFalse;
    }

    if (header[0] != Plug_info::framework_code) return Steinberg::kResultFalse;
    if (header[1] != Plug_info::manufacturer_code) return Steinberg::kResultFalse;
    if (header[2] != Plug_info::plugin_code) return Steinberg::kResultFalse;

    const auto num_stored_values = header[3];

    // Snapshot for host-load undo capture (knob space, pre-load).
    const auto before = _snapshot_knob_params();

    // Notify view (we perform the persistence check again here on the current model).
    auto notify = [this](auto& param, auto knob_value) {
        if (State_rules::is_persistent(param)) {
            setParamNormalized(param.identity.address, knob_value);
        }
    };

    auto read_and_notify = [&](const auto& knob_values, auto index) {
        // Do we have a real value?
        if (const auto knob_value = knob_values[index]; knob_value != State_rules::no_value) {
            const auto& spec = User_params::param_spec(index);
            notify(spec, knob_value);
        }
    };

    // Sized by what we can use, not by what the chunk claims; the stream is still read
    // in full so the bypass float below stays aligned.
    const auto usable_values = std::min<size_t>(num_stored_values, num_params);
    auto stored_values = std::vector<float>(usable_values);
    for (auto i = decltype(num_stored_values){}; i < num_stored_values; ++i) {
        auto value = float{};
        if (!streamer.readFloat(value)) {
            return Steinberg::kResultFalse;
        }
        if (i < num_params) stored_values[i] = value;
    }

    if (num_params <= num_stored_values) {
        // Set values stored in state.
        for (auto i = decltype(num_params){}; i < num_params; ++i) {
            read_and_notify(stored_values, i);
        }
    }
    else {
        // Set values stored in state.
        for (auto i = decltype(num_stored_values){}; i < num_stored_values; ++i) {
            read_and_notify(stored_values, i);
        }

        // Set remaining parameters to defaults.
        for (auto i = num_stored_values; i < num_params; ++i) {
            const auto& param = User_params::param_spec(i);
            const auto knob_value = Value_helper::default_value(param, Space::Knob);
            notify(param, knob_value);
        }
    }

    // Try to read the bypass parameter
    auto bypass_value = float{};
    if (streamer.readFloat(bypass_value)) {
        setParamNormalized(bypass_param_id, bypass_value);
    }
    else {
        //setParamNormalized(bypass_param_id, 0.f);
    }

    // Record the host load as one coalesced undo step (works editor open or closed).
    // The editor notify() fires at the end of setState (the second of VST3's two
    // restore calls), where the editor state has also arrived and this step is still
    // open for marker folding. Stash the load so setState can dispatch it.
    _host_load_after = _snapshot_knob_params();
    _host_load_changes.clear();
    _undo_history.push_host_load(before, _host_load_after, _host_load_changes);
    _host_load_pending = true;

    if (auto* handler = getComponentHandler()) {
        handler->restartComponent(Steinberg::Vst::kParamValuesChanged);
    }

    return Steinberg::kResultOk;
}

// MARK: - editor state

Steinberg::tresult PLUGIN_API Controller::setState(Steinberg::IBStream* state)
{
    // Here you get the state of the controller.
    if (!state) {
        return Steinberg::kResultFalse;
    }

    // Streamer convenience wrapper.
    auto streamer = Steinberg::IBStreamer{state};

    auto header = State_rules::Vst3::Header{};
    if (!streamer.readInt32uArray(header.data(), static_cast<int32_t>(header.size()))) {
        return Steinberg::kResultFalse;
    }

    // Validate for real, not just in debug; every count below is untrusted until checked.
    if (header[0] != Plug_info::framework_code) return Steinberg::kResultFalse;
    if (header[1] != Plug_info::manufacturer_code) return Steinberg::kResultFalse;
    if (header[2] != Plug_info::plugin_code) return Steinberg::kResultFalse;

    const auto num_stored_pairs = header[3];

    // Grows in slices as bytes actually arrive, so a garbage length costs one slice
    // instead of a multi-gigabyte resize.
    auto read_container = [&](auto& container) {
        auto num = uint32_t{};
        if (!streamer.readInt32u(num)) {
            return false;
        }

        constexpr auto slice_items = size_t{4096};
        container.clear();

        auto done = size_t{};
        while (done < num) {
            const auto want = std::min<size_t>(slice_items, num - done);
            container.resize(done + want);
            if (!streamer.readRaw(container.data() + done, static_cast<int32_t>(sizeof(container[0]) * want))) {
                return false;
            }
            done += want;
        }
        return true;
    };

    // Read editor state.
    auto edit_state = State_map{};
    for (auto i = decltype(num_stored_pairs){}; i < num_stored_pairs; ++i) {
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
            case State_tag::Bool: {
                auto v = bool{};
                if (streamer.readBool(v)) {
                    value = v;
                    break;
                }
                return Steinberg::kResultFalse;
            }
            case State_tag::Int: {
                auto v = int32_t{};
                if (streamer.readInt32(v)) {
                    value = v;
                    break;
                }
                return Steinberg::kResultFalse;
            }
            case State_tag::Double: {
                auto v = double{};
                if (streamer.readDouble(v)) {
                    value = v;
                    break;
                }
                return Steinberg::kResultFalse;
            }
            case State_tag::String: {
                auto v = std::string{};
                if (read_container(v)) {
                    value = std::move(v);
                    break;
                }
                return Steinberg::kResultFalse;
            }
            default: {
                assert(false && "Unknown editor state type.");
                return Steinberg::kResultFalse;
            }
        }

        edit_state.emplace(std::move(key), std::move(value));
    }

    // Prime the size cache from the framework-owned keys so the view opens pre-sized
    // (no preferred→saved flash), then strip them so the app editor never sees them.
    if (const auto size = editor_size_state::extract(edit_state)) {
        resized({size->first, size->second});
    }
    editor_size_state::strip(edit_state);

    _editor->load_state(edit_state);

    // Notify the editor of the host load synchronously, now that both restore calls
    // have landed. add_param folds any editor-owned marker into the load's single
    // undo step (still open from setComponentState).
    if (_host_load_pending) {
        auto add_param = [this](uint32_t addr, double knob) {
            if (addr >= num_params) return;
            const auto from = getParamNormalized(addr);
            setParamNormalized(addr, knob); // VST3 normalized == knob space.
            _undo_history.amend_host_load(addr, from, knob);
        };
        _editor->notify(Host_event{Host_preset_loaded{
            .changes = _host_load_changes,
            .params = _host_load_after,
            .add_param = add_param,
        }});
        _host_load_pending = false;
    }

    return Steinberg::kResultTrue;
}

//------------------------------------------------------------------------
Steinberg::tresult PLUGIN_API Controller::getState(Steinberg::IBStream* state)
{
    // Here you are asked to deliver the state of the controller (if needed).
    // Note: the real state of your plug-in is saved in the processor.
    if (!state) {
        return Steinberg::kResultFalse;
    }

    // Streamer convenience wrapper.
    auto streamer = Steinberg::IBStreamer{state};

    auto edit_state = _editor->save_state();

    // Inject the framework-owned editor window size (from our own cache) so the
    // window reopens pre-sized. The app editor never emits these keys.
    if (_last_size) {
        editor_size_state::inject(edit_state, _last_size->w, _last_size->h);
    }

    const auto num_editor_items = static_cast<uint32_t>(edit_state.size());

    const auto header = State_rules::Vst3::Header{
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
            case State_tag::Bool: {
               const auto value = std::get_if<bool>(&val);
               if (value && streamer.writeBool(*value)) {
                   break;
               }
               return Steinberg::kResultFalse;
            }
            case State_tag::Int: {
                const auto value = std::get_if<int32_t>(&val);
                if (value && streamer.writeInt32(*value)) {
                    break;
                }
                return Steinberg::kResultFalse;
            }
            case State_tag::Double: {
                const auto value = std::get_if<double>(&val);
                if (value && streamer.writeDouble(*value)) {
                    break;
                }
                return Steinberg::kResultFalse;
            }
            case State_tag::String: {
                const auto value = std::get_if<std::string>(&val);
                if (value && write_container(*value)) {
                    break;
                }
                return Steinberg::kResultFalse;
            }
            default: {
                assert(false && "Unknown editor state type.");
                return Steinberg::kResultFalse;
            }
        }
    }

    return Steinberg::kResultTrue;
}

//------------------------------------------------------------------------
Steinberg::tresult PLUGIN_API Controller::getParamStringByValue(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue valueNormalized, Steinberg::Vst::String128 string)
{
    using namespace params;

    if (tag == bypass_param_id) {
        const auto str = valueNormalized >= 0.5f ? "On" : "Off"; // Bypass
        Steinberg::Vst::StringConvert::convert(str, string);
        return Steinberg::kResultTrue;
    }

    // Called by host to get a string for given normalized value of a specific parameter.
    // (without having to set the value!)
    if (tag >= User_params::num_params) return Steinberg::kResultFalse;

    const auto& params = User_params::param_specs(Param_order::Indexable);
    const auto& param = params[tag];
    const auto host = Value_helper::knob_to_host(valueNormalized, param.semantics);
    const auto str = Host_formatter::to_string(host, param.semantics);
    Steinberg::Vst::StringConvert::convert(str, string);

    return Steinberg::kResultTrue;
}

//------------------------------------------------------------------------
Steinberg::tresult PLUGIN_API Controller::getParamValueByString(Steinberg::Vst::ParamID tag, Steinberg::Vst::TChar* string, Steinberg::Vst::ParamValue& valueNormalized)
{
    using namespace params;
    // Called by host to get a normalized value from a string representation of a specific parameter.
    // (without having to set the value!)
    if (tag >= User_params::num_params) return Steinberg::kResultFalse;

    const auto& param = User_params::param_spec(tag);
    const auto str = Steinberg::Vst::StringConvert::convert(string);
    if (const auto plain = Host_formatter::to_value(str, param.semantics)) {
        valueNormalized = Value_helper::plain_to_knob(*plain, param.semantics);
        return Steinberg::kResultTrue;
    }

    return Steinberg::kResultFalse;
}

Steinberg::Vst::ParamValue PLUGIN_API Controller::normalizedParamToPlain(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue valueNormalized)
{
    using namespace params;
    if (tag == bypass_param_id) {
        return valueNormalized >= 0.5f ? 1.f : 0.f; // Bypass
    }

    if (tag >= User_params::num_params) return 0.f;

    const auto& param = User_params::param_spec(tag);
    return Value_helper::knob_to_plain(valueNormalized, param.semantics);
}

Steinberg::Vst::ParamValue PLUGIN_API Controller::plainParamToNormalized(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue plainValue)
{
    using namespace params;
    if (tag == bypass_param_id) {
        return plainValue >= 0.5f ? 1.f : 0.f; // Bypass
    }

    if (tag >= User_params::num_params) return 0.f;

    const auto& param = User_params::param_spec(tag);
    return Value_helper::plain_to_knob(plainValue, param.semantics);
}

Steinberg::Vst::ParamValue PLUGIN_API Controller::getParamNormalized(Steinberg::Vst::ParamID tag)
{
    return Super::getParamNormalized(tag);
}

Steinberg::tresult PLUGIN_API Controller::setParamNormalized(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue value)
{
    const auto result = Super::setParamNormalized(tag, value);

    // Do not forward setParam to UI during gestures.
    if (_gestured.find(tag) != _gestured.end()) { return result; }

    // Is it a parameter?
    if (tag < num_params) {
        _state_queue.push(Set_param{.address = tag, .value = value});
    }
    // Is it a meter?
    else if (tag >= export_param_offset && tag < export_param_offset + num_meters) {
        const auto id = tag - export_param_offset;

        // Convert back to plain for UI.
        const auto& spec = User_meters::spec(id);
        const auto plain = norm_to_plain(value, spec.range);
        _mailbox.post(id, static_cast<float>(plain));
    }
    // Is it a latency change?
    else if (tag == latency_param_id) {
        if (auto* handler = getComponentHandler()) {
            handler->restartComponent(Steinberg::Vst::kLatencyChanged);
        }
    }

    return result;
}

Steinberg::tresult PLUGIN_API Controller::setComponentHandler(Steinberg::Vst::IComponentHandler* handler)
{
    const auto result = Super::setComponentHandler(handler);
    return result;
}

//------------------------------------------------------------------------
Steinberg::IPlugView* PLUGIN_API Controller::createView(Steinberg::FIDString name)
{
    // Here the Host wants to open your editor (if you have one).
    if (Steinberg::FIDStringsEqual(name, Steinberg::Vst::ViewType::kEditor))
    {
        // Make the UI connection.
        auto receiver = Ui_receiver{
            .get_param = [this](auto id) {
                return getParamNormalized(id);
            },
            .read_meters = [this](std::span<meters::Sample> out) {
                _mailbox.read(out);
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
                    [](const auto&) {}
            }, a);
            }
        };

        return new View{{
            .controller = this,
            .editor = &(*_editor),
            .receiver = std::move(receiver),
            .tasks = &_tasks,
            .undo_history = &_undo_history,
            .actions = &_actions,
#if TINY_HAS_WORKER
            .drain_worker_to_editor = [this]() { this->_drain_worker_to_editor(); }
#endif
        }};
    }

    return nullptr;
}

} // namespace tiny::vst3

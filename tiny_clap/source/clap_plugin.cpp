#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "clap_plugin.h"

// MARK: - plugin

namespace tiny {

bool Clap_plugin::init() noexcept
{
    return true;
}

bool Clap_plugin::activate(double sampleRate, uint32_t /*minFrameCount*/, uint32_t /*maxFrameCount*/) noexcept
{
    // Check whether kernel wants latency change *prior* to reset.
    const auto wants_latency_change = _kernel->wants_latency_change();

    _kernel->reset(sampleRate);

    // Change is now manifested.
    if (wants_latency_change) {
        auto* latency_ext = (const clap_host_latency_t*)_host->get_extension(_host, CLAP_EXT_LATENCY);
        if (latency_ext) latency_ext->changed(_host);
    }
    
    return true;
}

void Clap_plugin::deactivate() noexcept
{
}

bool Clap_plugin::startProcessing() noexcept
{
    return true;
}

void Clap_plugin::stopProcessing() noexcept
{
}

clap_process_status Clap_plugin::process(const clap_process* process) noexcept
{
    return _kernel->process(process);
}

void Clap_plugin::reset() noexcept
{
}

void Clap_plugin::onMainThread() noexcept
{
}

const void* Clap_plugin::extension(const char* /*id*/) noexcept
{
    return nullptr;
}

bool Clap_plugin::enableDraftExtensions() const noexcept
{
    return false;
}

// MARK: - state

bool Clap_plugin::stateSave(const clap_ostream* stream) noexcept
{
    if (!stream) return false;

    // Tree version.
    const auto tree_version = max_tree_version(_param_infos.tree());
    const auto header = State_header{
        Plug_info::framework_code, // Reserved
        Plug_info::manufacturer_code,
        Plug_info::plugin_code,
        tree_version
    };

    const auto expected = sizeof(header);
    const auto result = stream->write(stream, header.data(), expected);
    if (result != expected) {
        return false;
    }

    for (auto i = decltype(num_params){}; i < num_params; ++i) {
        const auto host_value = static_cast<float>(_kernel->get_host_value(i));
        const auto expected = sizeof(host_value);
        const auto result = stream->write(stream, &host_value, expected);
        if (result != expected) {
            return false;
        }
    }

    return true;
}

bool Clap_plugin::stateLoad(const clap_istream* stream) noexcept
{
    if (!stream) return false;

    auto header = State_header{};
    const auto expected = sizeof(header);
    const auto result = stream->read(stream, header.data(), expected);
    if (result != expected) {
        return false;
    }

    // Validate header.
    assert(header[0] == Plug_info::framework_code && "Unexpected framework code.");
    assert(header[1] == Plug_info::manufacturer_code && "Unexpected manufacturer code.");
    assert(header[2] == Plug_info::plugin_code && "Unexpected plug-in code.");
    assert(header[3] > 0 && "Unexpected tree version.");

    const auto tree_version = max_tree_version(_param_infos.tree());
    const auto state_version = header[3];

    // Notify kernel and view (if not an interface parameter).
    auto do_notify = [this](auto& param, auto knob_value) {
        if (param.policy != Host_policy::interface) {
            _kernel->handle_action(Set_param{.id = param.id, .value = knob_value});
            _view->set_param(param.id, knob_value);
        }
    };

    if (tree_version <= state_version) {
        // Implies "num params in tree" <= "num params in state"
        // We should be able to safely read `num_params` floats.
        for (auto i = decltype(num_params){}; i < num_params; ++i) {
            auto host_value = float{};
            const auto expected = sizeof(host_value);
            const auto result = stream->read(stream, &host_value, expected);
            if (result != expected) {
                return false;
            }

            // Convert to knob and send to action queue.
            const auto& param = _param_infos.param_for(i);
            const auto knob_value = Value_conv::host_to_knob(host_value, param.semantics);
            do_notify(param, knob_value);
        }
    }
    else if (tree_version > state_version) {
        // Implies "num params in tree" > "num params in state"
        const auto num_state = num_params_with_version(_param_infos.tree(), state_version);

        // Set values stored in state.
        for (auto i = decltype(num_state){}; i < num_state; ++i) {
            auto host_value = float{};
            const auto expected = sizeof(host_value);
            const auto result = stream->read(stream, &host_value, expected);
            if (result != expected) {
                return false;
            }

            // Convert to knob and send to action queue.
            const auto& param = _param_infos.param_for(i);
            const auto knob_value = Value_conv::host_to_knob(host_value, param.semantics);
            do_notify(param, knob_value);
        }

        // Set remaining parameters to defaults. 
        for (auto i = num_state; i < num_params; ++i) {
            const auto& param = _param_infos.param_for(i);
            const auto knob_value = get_knob_default(param);
            do_notify(param, knob_value);
        }
    }

    return true;
}

// MARK: - audio ports

bool Clap_plugin::implementsAudioPorts() const noexcept
{
    return true;
}

uint32_t Clap_plugin::audioPortsCount(bool isInput) const noexcept
{
    return isInput ? (Plug_info::wants_sidechain ? 2 : 1) : 1;
}

bool Clap_plugin::audioPortsInfo(uint32_t index, bool isInput, clap_audio_port_info* info) const noexcept
{
    if (!info) return false;

    const auto is_main = (index == 0);
    const char* port_name = isInput ? (is_main ? "Input" : "Sidechain") : "Output";

    *info = {};
    info->id = index;
    std::strncpy(info->name, port_name, CLAP_NAME_SIZE);
    info->flags = is_main ? CLAP_AUDIO_PORT_IS_MAIN : uint32_t{};
    info->channel_count = 2; // 
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;

    return true;
}

// MARK: - params

uint32_t Clap_plugin::paramsCount() const noexcept
{
    return num_params;
}

bool Clap_plugin::paramsInfo(uint32_t paramIndex, clap_param_info* info) const noexcept
{
    // The index is the order of appearance in the UI, and isn't necessarily the same as the id.
    if (paramIndex >= num_params || !info) return false;

    const auto& params = _param_infos.presentation_specs(); // Report params in presentation order!

    const auto& param = params[paramIndex];
    const auto& path = _modules[paramIndex];

    *info = {}; // Clear.
    info->id = param.id;
    info->flags = [policy = param.policy]() {
        using enum Host_policy;
        switch (policy) {
            case automation: return uint32_t{CLAP_PARAM_IS_AUTOMATABLE};
            case control: return uint32_t{}; // Do any hosts actually show a control here?
            case state: return uint32_t{CLAP_PARAM_IS_HIDDEN | CLAP_PARAM_IS_READONLY};
            case interface: return uint32_t{CLAP_PARAM_IS_HIDDEN | CLAP_PARAM_IS_READONLY};
        }
    }();
    info->cookie = nullptr;
    std::strncpy(info->name, param.name, CLAP_NAME_SIZE);
    std::strncpy(info->module, path.c_str(), CLAP_NAME_SIZE);

    // Set min, max, default based on semantics.
    std::visit(Inline_visitor{
        [&](const Bool_semantics& b) {
            info->flags |= CLAP_PARAM_IS_STEPPED;
            info->min_value = 0;
            info->max_value = 1;
            info->default_value = b.def_val ? 1 : 0;
        },
        [&](const List_semantics& l) {
            info->flags |= (CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_ENUM);
            info->min_value = 0;
            info->max_value = l.items.size() - 1;
            info->default_value = static_cast<double>(l.def_val);
        },
        [&](const Int_semantics& i) {
            info->flags |= CLAP_PARAM_IS_STEPPED;
            info->min_value = i.min_val;
            info->max_value = i.max_val;
            info->default_value = i.def_val;
        },
        [&](const Real_semantics& r) {
            info->min_value = 0;
            info->max_value = 1;
            info->default_value = plain_to_norm(r.def_val, r);
        },
    }, param.semantics);

    return true;
}

bool Clap_plugin::paramsValue(clap_id paramId, double* value) noexcept
{
    if (paramId >= num_params) return false;
    *value = _kernel->get_host_value(paramId);
    return true;
}

bool Clap_plugin::paramsValueToText(clap_id paramId, double value, char* display, uint32_t size) noexcept
{
    if (paramId >= num_params || !display) return false;

    const auto& param = _param_infos.param_for(paramId);
    const auto str = Host_formatter::format_string(value, param.semantics);
    std::strncpy(display, str.c_str(), size);
    display[size - 1] = '\0'; // In case str is longer than display.

    return true;
}

bool Clap_plugin::paramsTextToValue(clap_id paramId, const char* display, double* value) noexcept
{
    if (paramId >= num_params || !display) return false;

    const auto& param = _param_infos.param_for(paramId);
    const auto str = std::string{display};

    if (const auto plain = Host_formatter::format_value(str, param.semantics)) {
        *value = Value_conv::plain_to_host(*plain, param.semantics);
        return true;
    }

    return false;
}

void Clap_plugin::paramsFlush(const clap_input_events* in, const clap_output_events* /*out*/) noexcept
{
    if (!in) return;

    const auto size = in->size(in);

    for (auto i = decltype(size){}; i < size; ++i) {
        const auto* event = in->get(in, i);
        _kernel->handle_flushed(event);
    }
}

// MARK: - gui

bool Clap_plugin::guiIsApiSupported(const char* api, bool isFloating) noexcept
{
    return !isFloating && strcmp(api, gui_preferred_api) == 0;
}

bool Clap_plugin::guiGetPreferredApi(const char** api, bool* isFloating) noexcept
{
    *api = gui_preferred_api;
    *isFloating = false;
    return true;
}

bool Clap_plugin::guiCreate(const char* /*api*/, bool /*isFloating*/) noexcept
{
    _view->create();
    return true;
}

void Clap_plugin::guiDestroy() noexcept
{
    _view->destroy();
}

bool Clap_plugin::guiSetScale(double /*scale*/) noexcept
{
    return true;
}

bool Clap_plugin::guiShow() noexcept
{
    return true;
}

bool Clap_plugin::guiHide() noexcept
{
    return true;
}

bool Clap_plugin::guiGetSize(uint32_t* width, uint32_t* height) noexcept
{
    _view->get_size(width, height);
    return true;
}

bool Clap_plugin::guiCanResize() const noexcept
{
    return true;
}

bool Clap_plugin::guiGetResizeHints(clap_gui_resize_hints_t* /*hints*/) noexcept
{
    // *hints = {
    //     .can_resize_horizontally = true,
    //     .can_resize_vertically = true,
    //     .preserve_aspect_ratio = false,
    //     .aspect_ratio_width = 0,
    //     .aspect_ratio_height = 0
    // };
    return true;
}

bool Clap_plugin::guiAdjustSize(uint32_t* /*width*/, uint32_t* /*height*/) noexcept
{
    return true;
}

bool Clap_plugin::guiSetSize(uint32_t width, uint32_t height) noexcept
{
    return _view->set_size(width, height);
}

void Clap_plugin::guiSuggestTitle(const char* /*title*/) noexcept
{
    // floating only
}

bool Clap_plugin::guiSetParent(const clap_window* window) noexcept
{
    return _view->set_parent(window);
}

bool Clap_plugin::guiSetTransient(const clap_window* /*window*/) noexcept
{
    return false; // floating only
}

uint32_t Clap_plugin::latencyGet() const noexcept
{
    return _kernel->get_latency();
}

} // namespace tiny
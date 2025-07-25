#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "clap/helpers/plugin.hh"
#include "clap/helpers/plugin.hxx"
#include "clap/helpers/host-proxy.hh"
#include "clap/helpers/host-proxy.hxx"

#include "clap_plugin.h"

Clap_plugin::Clap_plugin(const clap_host* host) : Super(&descriptor, host)
{
    // Figure out the module paths for our parameters.
    const auto& tree = _params.get_tree();
    _modules = tiny::clap::tree_to_clap_modules(tree);
}

Clap_plugin::~Clap_plugin()
{
}

// MARK: - plugin

bool Clap_plugin::init() noexcept
{
    return true;
}

bool Clap_plugin::activate(double sampleRate, uint32_t /*minFrameCount*/, uint32_t maxFrameCount) noexcept
{
    _kernel->reset(sampleRate, maxFrameCount);
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

// MARK: - audio ports

bool Clap_plugin::implementsAudioPorts() const noexcept
{
    return true;
}

uint32_t Clap_plugin::audioPortsCount(bool isInput) const noexcept
{
    using namespace tiny;
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
    return User_params::num_params;
}

bool Clap_plugin::paramsInfo(uint32_t paramIndex, clap_param_info* info) const noexcept
{
    // The index is the order of appearance in the UI, and isn't necessarily the same as the id.
    if (paramIndex >= User_params::num_params || !info) return false;

    using namespace tiny;
    
    const auto& params = _params.get_presentation_specs(); // Report params in presentation order!

    const auto& param = params[paramIndex];
    const auto& path = _modules[paramIndex];

    auto resolve_flags = [](const User_params::Spec& param) {
        auto result = clap_param_info_flags{};
        
        if (param.hidden) {
            result |= CLAP_PARAM_IS_HIDDEN;
        }
        else {
            result |= CLAP_PARAM_IS_AUTOMATABLE;
        }

        return result;
    };

    *info = {};
    info->id = to_underlying(param.id);
    info->flags = resolve_flags(param);
    info->cookie = nullptr;
    std::strncpy(info->name, param.name, CLAP_NAME_SIZE);
    std::strncpy(info->module, path.c_str(), CLAP_NAME_SIZE);

    // Set min, max, default based on semantics.
    std::visit(
        Inline_visitor{
            [&](const Bool_semantics& b) {
                info->flags |= CLAP_PARAM_IS_STEPPED;
                info->min_value = 0;
                info->max_value = 1;
                info->default_value = b.def_val ? 1 : 0;
            },
            [&](const List_semantics& l) {
                info->flags |= (CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_ENUM);
                info->min_value = 0;
                info->max_value = l.labels.size() - 1;
                info->default_value = static_cast<double>(l.def_val);
            },
            [&](const Float_semantics& f) {
                info->min_value = 0;
                info->max_value = 1;
                info->default_value = f.knob_adapter.plain_to_norm(f, f.def_val);
            },
            [&](const Int_semantics& i) {
                info->flags |= CLAP_PARAM_IS_STEPPED;
                info->min_value = i.min_val;
                info->max_value = i.max_val;
                info->default_value = i.def_val;
            }
        }
    , param.semantics);

    return true;
}

bool Clap_plugin::paramsValue(clap_id paramId, double* value) noexcept
{
    if (paramId > User_params::num_params || !value) return false;
    *value = _kernel->get_host_value(paramId);
    return true;
}

bool Clap_plugin::paramsValueToText(clap_id paramId, double value, char* display, uint32_t size) noexcept
{
    if (paramId >= User_params::num_params || !display) return false;

    using namespace tiny;
    const auto& params = _params.get_kernel_specs();
    const auto& param = params[paramId];
    const auto str = Host_formatter::format_string(value, param.semantics);
    std::strncpy(display, str.c_str(), size);
    display[size - 1] = '\0'; // In case str is longer than display.

    return true;
}

bool Clap_plugin::paramsTextToValue(clap_id paramId, const char* display, double* value) noexcept
{
    if (paramId >= User_params::num_params || !display || !value) return false;

    using namespace tiny;
    const auto& params = _params.get_kernel_specs();
    const auto& param = params[paramId];
    const auto str = std::string{display};

    if (const auto plain = Host_formatter::format_value(str, param.semantics)) {
        *value = plain_to_host_space(*plain, param);
        return true;
    }

    return false;
}

void Clap_plugin::paramsFlush(const clap_input_events* in, const clap_output_events* /*out*/) noexcept
{
    if (!in) return;

    const auto size = in->size(in);

    for (size_t i = 0; i < size; ++i) {
        const auto* event = in->get(in, i);
        _kernel->handle_event(event);
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
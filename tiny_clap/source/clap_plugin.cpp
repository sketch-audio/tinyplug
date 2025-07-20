#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "clap/helpers/plugin.hh"
#include "clap/helpers/plugin.hxx"
#include "clap/helpers/host-proxy.hh"
#include "clap/helpers/host-proxy.hxx"

#include "clap_adapters.h"
#include "clap_plugin.h"

#include "platform/platform_view.h"

Clap_plugin::Clap_plugin(const clap_host* host) : Super(&descriptor, host)
{
    using namespace tiny;
    const auto tree = Param_model::build_tree();
    _specs = params::flatten_tree(tree);
    params::sort_param_specs_by_id(_specs);

    for (const auto& param : _specs) {
        const auto def_val = params::get_host_default(param);
        _hostvalues[utils::to_underlying(param.id)] = def_val;
    }
}

Clap_plugin::~Clap_plugin()
{
}

// MARK: - plugin

bool Clap_plugin::init() noexcept
{
    return true;
}

bool Clap_plugin::activate(double /*sampleRate*/, uint32_t /*minFrameCount*/, uint32_t /*maxFrameCount*/) noexcept
{
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
    // Get ready to process the input events.
    const auto* in_events = process->in_events;
    const auto event_count = in_events->size(in_events);

    auto event_index = size_t{};
    const auto* event = event_count > 0 ? in_events->get(in_events, event_index) : nullptr;

    auto go_to_next_event = [&]() {
        ++event_index;
        if (event_index >= event_count) {
            event = nullptr;
        }
        else {
            event = in_events->get(in_events, event_index);
        }
    };

    for (size_t i = 0; i < process->frames_count; ++i) {
        // Process the input events.
        while (event && event->time == i) {
            this->handle_event(event);
            go_to_next_event();
        }
    }
    return CLAP_PROCESS_SLEEP;
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
    return tiny::Param_model::num_params;
}

bool Clap_plugin::paramsInfo(uint32_t paramIndex, clap_param_info* info) const noexcept
{
    // The index is the order of appearance in the UI, and isn't necessarily the same as the id.
    if (paramIndex >= tiny::Param_model::num_params || !info) return false;

    using namespace tiny;
    static const auto tree = Param_model::build_tree();
    static const auto flat_map = params::flatten_tree(tree);
    static const auto paths = tiny::clap::flatten_tree_paths(tree);

    const auto& param = flat_map[paramIndex];
    const auto& path = paths[paramIndex];

    auto resolve_flags = [](const Param_model::Spec& param) {
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
    info->id = utils::to_underlying(param.id);
    info->flags = resolve_flags(param);
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
    }, param.semantics);

    return true;
}

bool Clap_plugin::paramsValue(clap_id paramId, double* value) noexcept
{
    if (paramId > tiny::Param_model::num_params || !value) return false;
    *value = _hostvalues[paramId];
    return true;
}

bool Clap_plugin::paramsValueToText(clap_id paramId, double value, char* display, uint32_t size) noexcept
{
    if (paramId >= tiny::Param_model::num_params || !display) return false;

    using namespace tiny;
    const auto& param = _specs[paramId];
    const auto str = Param_model::format_string(value, param, _hostvalues);
    std::strncpy(display, str.c_str(), size);
    display[size - 1] = '\0'; // In case str is longer than display.

    return true;
}

bool Clap_plugin::paramsTextToValue(clap_id paramId, const char* display, double* value) noexcept
{
    if (paramId >= tiny::Param_model::num_params || !display || !value) return false;

    using namespace tiny;
    const auto& param = _specs[paramId];
    const auto str = std::string{display};

    if (const auto plain = Param_model::format_value(str, param)) {
        *value = params::plain_to_host_space(*plain, param);
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
        this->handle_event(event);
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
    platform_view = Platform_views::make_owning(_delegate);
    return true;
}

void Clap_plugin::guiDestroy() noexcept
{
    platform_view = nullptr;
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
    const auto delegate_size = _delegate->getSize();
    *width = delegate_size.width;
    *height = delegate_size.height;
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
    if (!platform_view) return false;

    _delegate->onResize({static_cast<int>(width), static_cast<int>(height)});
    platform_view->resize(width, height);
    return true;
}

void Clap_plugin::guiSuggestTitle(const char* /*title*/) noexcept
{
    // floating only
}

bool Clap_plugin::guiSetParent(const clap_window* window) noexcept
{
    if (!platform_view) return false;
    
    // Resolve the platform window type.
    auto* platform_window = [=]() {
        if (Platform::resolved == Platform::Type::macos) {
            return window->cocoa;
        } else if (Platform::resolved == Platform::Type::windows) {
            return window->win32;
        }
    }();
    platform_view->receive_parent(platform_window);
    return true;
}

bool Clap_plugin::guiSetTransient(const clap_window* /*window*/) noexcept
{
    return false; // floating only
}

// MARK: - private

auto Clap_plugin::handle_event(const clap_event_header* event) -> void
{
    if (event->space_id != CLAP_CORE_EVENT_SPACE_ID) return;

    switch (event->type) {
        case CLAP_EVENT_PARAM_VALUE: {
            const auto* value_event = reinterpret_cast<const clap_event_param_value*>(event);
            const auto id = value_event->param_id;
            const auto value = value_event->value;
            _hostvalues[id] = value;
            break;
        }
    }
}
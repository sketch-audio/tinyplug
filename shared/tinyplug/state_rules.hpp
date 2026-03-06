#pragma once

#include <array>
#include <limits>
#include <optional>
#include <vector>

#include "tiny_params.h"

namespace tiny {

template<typename T>
using Maybe_values = std::vector<std::optional<T>>;

struct State_rules {
    // 
    static constexpr auto no_value = std::numeric_limits<float>::lowest();

    static auto is_persistent(const Param_spec& spec) -> bool
    {
        return spec.policy != Host_policy::interface;
    }

    /**
     * In AAX, we have our own chunk ID and attach some of our own keys to it.
    */
    struct Aax {
        static constexpr auto chunk_id = uint32_t{'tiny'}; // AAX_CTypeID;
        static constexpr const char num_params[] = "tinyplug-num-params";
        static constexpr const char edit_keys[] = "tinyplug-edit-keys";
        static constexpr const char host_bypass[] = "tinyplug-host-bypass";
    };

    /**
     * In AUv2, we attach some of our own keys to the state dictionary.
    */
    struct Auv2 {
        static constexpr const char num_params[] = "tinyplug-num-params";
        static constexpr const char num_editor_items[] = "tinyplug-num-editor-items";
        static constexpr const char editor_state_map[] = "tinyplug-editor-state-map";
    };

    /**
     * Basically do the same thing in AUv3 as in AUv2.
     */
    struct Auv3 {
        static constexpr const char num_params[] = "tinyplug-num-params";
        static constexpr const char num_editor_items[] = "tinyplug-num-editor-items";
        static constexpr const char editor_state_map[] = "tinyplug-editor-state-map";
        static constexpr const char values_from_preset[] = "tinyplug-values-from-preset"; // Optional
    };

    /**
     * In CLAP, the parameter values and editor state are stored together. The parameter values are
     * first, followed immediately by the key-value pairs of the editor state. The header indicates
     * how many of each there are.
     *
     * Header:
     * - Framework code: `Plug_info::framework_code`
     * - Manufacturer code: `Plug_info::manufacturer_code`
     * - Plug-in code: `Plug_info::plugin_code`
     * - Number of parameter values in host space (`float`)
     * - Number of key-value pairs (`State_item`)
    */
    struct Clap {
        static constexpr auto num_header_items = size_t{5};
        using Header = std::array<uint32_t, num_header_items>;
    };

    /**
     * In VST3, the parameter values are stored by the processor and the editor state is stored by
     * the controller. We use a shared header structure to indicate how many values we stored in
     * each part.
     *
     * Header:
     * - Framework code: `Plug_info::framework_code`
     * - Manufacturer code: `Plug_info::manufacturer_code`
     * - Plug-in code: `Plug_info::plugin_code`
     * - Number of items:
     *   -- Processor: number of parameter values in knob space (`float`)
     *   -- Controller: number of key-value pairs (`State_item`)
    */
    struct Vst3 {
        static constexpr auto num_header_items = size_t{4};
        using Header = std::array<uint32_t, num_header_items>;
    };
};

} // namespace tiny

#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "tiny_layout.h"
#include "tiny_params.h"

namespace tiny {

struct Plug_io {
    struct Port_counts {
        uint32_t num_inputs{};
        uint32_t num_outputs{};
    };

    Port_counts audio_ports{};
    Port_counts midi_ports{};

    static auto resolve_audio_input_name(uint32_t index, uint32_t count) -> std::string
    {
        if (index >= count)
            return {};

        if (count == 1)
            return "Input";

        // Two inputs (primary & sidechain)
        if (count == 2)
            return (index == 0) ? "Input" : "Sidechain";

        // Many inputs
        return "Input " + std::to_string(index + 1);
    }

    static auto resolve_audio_output_name(uint32_t index, uint32_t count) -> std::string
    {
        if (index >= count)
            return {};

        if (count == 1)
            return "Output";

        // Many outputs
        return "Output " + std::to_string(index + 1);
    }
};

} // namespace tiny
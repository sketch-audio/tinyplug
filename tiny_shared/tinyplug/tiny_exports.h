#pragma once

#include <array>
#include <cstdint>

#include "tiny_params.h"
#include "tiny_utils.h"

namespace tiny {

// Defines how the framework should process and deliver your kernel's exports to your custom view.
enum class Export_type : uint32_t {
    // Your custom view receives the maximum unconsumed value.
    peak = 0,

    // Your custom view receives the most recent value.
    stream,

    // Your custom view receives `double{1}` for exactly one frame (otherwise `double{}`).
    // Duplicate trigs for a given frame are coalesced to a single event.
    trig
};

struct Tagged_export {
    double value{};
    bool updated{}; // Have we updated the peak/stream value this frame?
    bool trigged{}; // Have we received a trig for this frame?
};

static constexpr auto EXPORT_OFFSET = int32_t{1 << 30};

template<Some_param_model User_model>
struct Exports {

    static constexpr auto num_exports = enum_raw(User_model::Export_id::num_exports);

    static auto type_for(uint32_t index) -> Export_type
    {
        const auto id = static_cast<typename User_model::Export_id>(index);
        return User_model::export_type(id);
    }

};

}
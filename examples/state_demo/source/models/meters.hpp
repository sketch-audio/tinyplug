#pragma once

#include <tiny_core/tiny_core.hpp>

namespace tiny::models {

struct Meters {
    enum class Address : std::uint32_t {
        Step = 0, // The step the processor is playing, for the editor's playhead.
        Num_meters
    };
    static constexpr auto num_meters = enum_raw(Address::Num_meters);

    static auto make_spec(std::uint32_t) -> meters::Spec
    {
        return {.range = {.min_val = 0, .max_val = 15}, .policy = meters::Policy::Stream};
    }
};
static_assert(meters::Model<Meters>);

} // namespace tiny::models

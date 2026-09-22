#pragma once

#include <tiny_core/tiny_core.hpp>

namespace tiny::models {

// Minimal worker demo. Exercises every leg of the channel symmetrically:
//   From_processor: Tick (sample-position snapshot)
//   From_editor:    Set_session (UUID string)
//   To_processor:   Set_counter (round-trip example)
//   To_editor:      Session_path (string the worker would derive)

struct Tick {
    int64_t sample_pos{};
};

struct Set_session {
    std::array<char, 64> uuid{};
};

struct Set_counter {
    uint64_t count{};
};

struct Session_path {
    std::array<char, 128> path{};
};

// The channel shape: the four typed message variants plus capacity/period tuning.
struct Work {
    using From_processor = std::variant<Tick>;
    using From_editor    = std::variant<Set_session>;
    using To_processor   = std::variant<Set_counter>;
    using To_editor      = std::variant<Session_path>;

    static constexpr auto inbound_capacity  = size_t{64};
    static constexpr auto outbound_capacity = size_t{16};
    static constexpr auto update_period = std::chrono::milliseconds{16};
};
static_assert(work::Model<Work>);

} // namespace tiny::models

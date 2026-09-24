#pragma once

#include <array>

#include <tiny_core/tiny_core.hpp>

namespace tiny::models {

// A waveshaper transfer curve over -1…1, what the worker designs for the processor.
inline constexpr auto curve_points = size_t{65};
using Curve_table = std::array<float, curve_points>;

// Processor -> worker: a curve for this drive, please (sent when Drive moves) and a heartbeat.
struct Design { uint32_t seq{}; double drive{}; };
struct Heartbeat { uint32_t seq{}; int64_t frame{}; };

// Worker -> processor: the designed curve, and the heartbeat echoed back.
struct Curve { uint32_t seq{}; double drive{}; Curve_table table{}; };
struct Echo { uint32_t seq{}; int64_t frame{}; };

// Editor -> worker -> editor: a ping carrying when it left, and its pong.
struct Ping { uint32_t seq{}; int64_t sent_ns{}; };
struct Pong { uint32_t seq{}; int64_t sent_ns{}; uint32_t designs{}; };

// Each of the four channels carries its own message types.
struct Work {
    using From_processor = std::variant<Design, Heartbeat>;
    using From_editor = std::variant<Ping>;
    using To_processor = std::variant<Curve, Echo>;
    using To_editor = std::variant<Pong>;

    static constexpr auto inbound_capacity = size_t{64};
    static constexpr auto outbound_capacity = size_t{16};
    static constexpr auto update_period = std::chrono::milliseconds{10};
};
static_assert(work::Model<Work>);

} // namespace tiny::models

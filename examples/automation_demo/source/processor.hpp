#pragma once

#include <array>
#include <optional>

#include <tinyplug/tinyplug.hpp>

namespace tiny::process {

// Records everything the host says to the processor, in order, with the frame it landed at:
// parameter sets and ramps, `configure`, every reset and the render mode (realtime or offline). The editor draws the realized Value
// and the log; with logging compiled in (Debug), each entry also goes to the tinyplug log
// (category `params`). With DC Out on, the output is the realized Value, so a bounce is the
// automation curve; off, audio passes through.
class Processor {
public:

    auto configure(const Config& config) -> void;
    auto reset(const Reset::Any& reset) -> void;
    auto handle(const Event::Any& event) -> void;
    auto process(Dsp_context& context) -> void;

    auto latency_samps() const -> uint32_t { return 0; }
    auto tail_samps() const -> uint32_t { return 0; }

private:

    using Address = models::Params::Address;
    using Block = models::Blocks::Address;
    using Kind = models::Log_entry::Kind;
    static constexpr auto num_params = User_params::num_params;

    // A host ramp, sample for sample: `count` steps of `inc`, then exactly the target.
    struct Ramp {
        float value{};
        float target{};
        float inc{};
        int32_t left{};

        auto snap(float to) -> void { value = target = to; left = 0; }
        auto settle() -> void { value = target; left = 0; }
        auto start(float to, int32_t frames) -> void
        {
            if (frames <= 0) return snap(to);
            target = to;
            inc = (to - value) / static_cast<float>(frames);
            left = frames;
        }
        auto next() -> float
        {
            value = left > 0 ? value + inc : target;
            left = left > 0 ? left - 1 : 0;
            return value;
        }
    };

    using enum tiny::params::Space;
    std::array<float, num_params> _values{tiny::params::make_defaults<float, User_params>(Plain)};
    Ramp _value{};

    int64_t _clock{}; // Frames since `configure`.
    std::optional<Render_mode> _render{}; // Logged on the first block and at every change.
    models::Log_frame _log{};
    models::Trace_frame _trace{};
    float _lo{1.f};
    float _hi{0.f};
    int32_t _filled{};

    auto _record(Kind kind, uint32_t address, double value, int32_t dur = 0) -> void;
    auto _trace_sample(float value, int64_t at) -> void;

};
static_assert(Interface<Processor>);

} // namespace tiny::process

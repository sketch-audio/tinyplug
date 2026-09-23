#pragma once

#include <array>
#include <optional>

#include <tinyplug/tinyplug.hpp>

namespace tiny::process {

// Plays the pattern in sixteenths while the host transport moves, holding each note for Gate
// of its step. Notes coming in pass straight through, so it can sit in front of an instrument
// a player is also playing. Built as an instrument that emits notes, with silent audio: see
// CMakeLists.txt.
class Processor {
public:

    auto configure(const Config& config) -> void;
    auto reset(const Reset::Any& reset) -> void;
    auto handle(const Event::Any& event) -> void;
    auto handle(const Note::Any& note) -> void;
    auto handle(const Control::Any& control) -> void;
    auto process(Dsp_context& context) -> void;

    auto latency_samps() const -> uint32_t { return 0; }
    auto tail_samps() const -> uint32_t { return 0; }

private:

    using Address = models::Params::Address;
    using Meter = models::Meters::Address;
    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_steps = models::State::num_steps;

    // Our own note ids: the high bit keeps them apart from notes passing through.
    static constexpr auto own_id = uint32_t{0x80000000};

    struct Held {
        Note::Id note{};
        double off_beat{}; // When its gate closes.
    };

    using enum tiny::params::Space;
    std::array<float, num_params> _values{tiny::params::make_defaults<float, User_params>(Plain)};

    double _sr{48000.};
    std::optional<int64_t> _last_step{}; // The sixteenth last started; none while stopped.
    std::optional<Held> _held{};
    uint32_t _next_id{};

    // What arrived between slices, sent at the top of the next one: the wrapper slices at every
    // incoming event, so frame 0 of the next `process` is exactly when it arrived.
    std::array<Performance, 64> _through{};
    size_t _num_through{};

    auto _pass(const Performance& event) -> void;
    auto _release(Dsp_context& context, int64_t frame) -> void;

};
static_assert(Interface<Processor>);

} // namespace tiny::process

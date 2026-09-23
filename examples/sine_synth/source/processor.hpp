#pragma once

#include <array>

#include <tinyplug/tinyplug.hpp>

namespace tiny::process {

// Eight sine voices with an ADSR. Pressure on a note (aftertouch, or a finger pushed up the
// on-screen key) deepens that note's vibrato; tuning slides it; brightness above centre adds
// the octave; bend, the mod wheel and sustain act on every note. MPE-capable.
class Processor {
public:

    auto configure(const Config& config) -> void;
    auto reset(const Reset::Any& reset) -> void;
    auto handle(const Event::Any& event) -> void;
    auto handle(const Note::Any& note) -> void;
    auto handle(const Control::Any& control) -> void;
    auto process(Dsp_context& context) -> void;

    auto latency_samps() const -> uint32_t { return 0; }
    auto tail_samps() const -> uint32_t;
    auto mpe_enabled() const -> bool { return _values[enum_raw(Address::Mpe)] >= 0.5f; }

private:

    using Address = models::Params::Address;
    using Block = models::Blocks::Address;
    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_voices = size_t{8};

    enum class Stage : uint8_t { Off, Attack, Decay, Sustain, Release };

    struct Voice {
        Stage stage{Stage::Off};
        uint32_t id{};
        uint8_t key{};
        bool held{};       // Key down; a sustained voice has let go of its key but not its sound.
        float velocity{};
        float level{};     // Envelope.
        float pressure{};
        float tuning{};    // Semitones.
        float brightness{0.5f};
        double phase{};
        uint64_t started{}; // For stealing the oldest.
    };

    using enum tiny::params::Space;
    std::array<float, num_params> _values{tiny::params::make_defaults<float, User_params>(Plain)};
    std::array<Voice, num_voices> _voices{};

    double _sr{48000.};
    double _vibrato_phase{};
    uint64_t _clock{};
    float _bend{};      // Semitones.
    float _mod_wheel{};
    bool _sustain{};

    auto _voice(uint32_t id) -> Voice*;
    auto _release(Voice& voice) -> void;
    auto _silence() -> void;
    auto _rate(Address address) const -> float; // Per-sample envelope step for a time parameter.

};
static_assert(Interface<Processor>);

} // namespace tiny::process

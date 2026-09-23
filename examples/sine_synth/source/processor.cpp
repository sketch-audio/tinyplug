#include "processor.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace tiny::process {

namespace {

constexpr auto bend_range = 2.f;      // Semitones at full bend.
constexpr auto vibrato_hz = 5.5;
constexpr auto two_pi = 2. * std::numbers::pi;

} // namespace

auto Processor::configure(const Config& config) -> void
{
    for (auto i = size_t{}; i < num_params; ++i) _values[i] = static_cast<float>(config.params[i]);
    _sr = config.sr;
    _silence();
}

auto Processor::reset(const Reset::Any& reset) -> void
{
    std::visit(Inline_visitor{
        [this](const Reset::Hard&) { _silence(); }, // Every voice released, every control back to rest.
        [](const Reset::Soft&) {},
        [](const Reset::Latency&) {}
    }, reset);
}

auto Processor::handle(const Event::Any& event) -> void
{
    std::visit(Inline_visitor{
        [this](const Event::Set& e) { _values[e.address] = static_cast<float>(e.value); },
        [this](const Event::Ramp& e) { _values[e.address] = static_cast<float>(e.target); },
    }, event);
}

auto Processor::handle(const Note::Any& note) -> void
{
    std::visit(Inline_visitor{
        [this](const Note::On& e) {
            // A free voice, else the oldest.
            auto* voice = &_voices[0];
            for (auto& v : _voices) {
                if (v.stage == Stage::Off) { voice = &v; break; }
                if (v.started < voice->started) voice = &v;
            }
            *voice = Voice{
                .stage = Stage::Attack, .id = e.note.id, .key = e.note.key, .held = true,
                .velocity = e.velocity, .level = 0.f, .phase = 0., .started = ++_clock,
            };
        },
        [this](const Note::Off& e) {
            if (auto* voice = _voice(e.note.id)) {
                voice->held = false;
                if (!_sustain) _release(*voice);
            }
        },
        [this](const Note::Choke& e) {
            if (auto* voice = _voice(e.note.id)) voice->stage = Stage::Off;
        },
        [this](const Note::Expression& e) {
            auto* voice = _voice(e.note.id);
            if (!voice) return;
            using enum Note::Expression::Kind;
            if (e.kind == Pressure) voice->pressure = static_cast<float>(std::clamp(e.value, 0., 1.));
            if (e.kind == Tuning) voice->tuning = static_cast<float>(e.value);
            if (e.kind == Brightness) voice->brightness = static_cast<float>(std::clamp(e.value, 0., 1.));
        },
    }, note);
}

auto Processor::handle(const Control::Any& control) -> void
{
    std::visit(Inline_visitor{
        [this](const Control::Bend& e) { _bend = static_cast<float>(e.value) * bend_range; },
        [](const Control::Pressure&) {},
        [this](const Control::Pedal& e) {
            using enum Control::Pedal::Kind;
            if (e.kind == Mod_wheel) _mod_wheel = static_cast<float>(e.value);
            if (e.kind == Sustain) {
                _sustain = e.value >= 0.5;
                if (!_sustain) {
                    for (auto& voice : _voices) {
                        if (!voice.held) _release(voice); // Let go of what the pedal was holding.
                    }
                }
            }
        },
    }, control);
}

auto Processor::process(Dsp_context& context) -> void
{
    const auto attack = _rate(Address::Attack);
    const auto decay = _rate(Address::Decay);
    const auto release = _rate(Address::Release);
    const auto sustain = _values[enum_raw(Address::Sustain)];
    const auto depth = _values[enum_raw(Address::Vibrato)];
    const auto level = _values[enum_raw(Address::Level)];
    const auto vibrato_step = two_pi * vibrato_hz / _sr;

    for (auto frame = size_t{}; frame < context.num_frames; ++frame) {
        const auto wobble = static_cast<float>(std::sin(_vibrato_phase));
        _vibrato_phase = std::fmod(_vibrato_phase + vibrato_step, two_pi);

        auto out = 0.f;
        for (auto& voice : _voices) {
            if (voice.stage == Stage::Off) continue;

            switch (voice.stage) {
                case Stage::Attack:
                    voice.level += attack;
                    if (voice.level >= 1.f) { voice.level = 1.f; voice.stage = Stage::Decay; }
                    break;
                case Stage::Decay:
                    voice.level -= decay * (1.f - sustain);
                    if (voice.level <= sustain) { voice.level = sustain; voice.stage = Stage::Sustain; }
                    break;
                case Stage::Release:
                    voice.level -= release;
                    if (voice.level <= 0.f) { voice.level = 0.f; voice.stage = Stage::Off; }
                    break;
                default:
                    break;
            }

            const auto amount = std::max(voice.pressure, _mod_wheel);
            const auto semis = static_cast<float>(voice.key) - 69.f + _bend + voice.tuning + depth * amount * wobble;
            const auto hz = 440. * std::exp2(static_cast<double>(semis) / 12.);
            voice.phase = std::fmod(voice.phase + two_pi * hz / _sr, two_pi);
            const auto octave = std::max(voice.brightness - 0.5f, 0.f); // Up to half as loud as the fundamental.
            const auto tone = std::sin(voice.phase) + static_cast<double>(octave) * std::sin(2. * voice.phase);
            out += static_cast<float>(tone) * voice.level * voice.velocity;
        }

        const auto sample = std::tanh(out * level * 0.5f);
        for (auto* channel : context.obuffers) channel[frame] = sample;
    }

    // What every key is doing, for the editor's keyboard.
    auto& keys = context.blocks.write<Block::Keys>();
    keys.level.fill(0);
    for (const auto& voice : _voices) {
        if (voice.stage == Stage::Off || voice.stage == Stage::Release) continue;
        keys.level[voice.key] = static_cast<uint8_t>(1.f + std::clamp(voice.pressure, 0.f, 1.f) * 254.f);
    }
    context.blocks.publish<Block::Keys>();
}

auto Processor::tail_samps() const -> uint32_t
{
    return static_cast<uint32_t>(_values[enum_raw(Address::Release)] * 0.001f * static_cast<float>(_sr));
}

auto Processor::_voice(uint32_t id) -> Voice*
{
    for (auto& voice : _voices) {
        if (voice.stage != Stage::Off && voice.id == id) return &voice;
    }
    return nullptr;
}

auto Processor::_release(Voice& voice) -> void
{
    if (voice.stage != Stage::Off) voice.stage = Stage::Release;
}

auto Processor::_silence() -> void
{
    for (auto& voice : _voices) voice.stage = Stage::Off;
    _bend = 0.f;
    _mod_wheel = 0.f;
    _sustain = false;
}

auto Processor::_rate(Address address) const -> float
{
    const auto ms = std::max(_values[enum_raw(address)], 1.f);
    return 1000.f / (ms * static_cast<float>(_sr));
}

} // namespace tiny::process

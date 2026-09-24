#pragma once

#include <cmath>

#include <tinyplug/tinyplug.hpp>

namespace tiny::work {

// Off the audio thread, so it may take its time: designs curves for the processor and answers
// heartbeats and pings. In VST3 it lives with the controller, so the processor's messages cross
// the component boundary on the way.
class Worker {
public:

    using From_processor = User_work::From_processor;
    using From_editor = User_work::From_editor;

    explicit Worker(Worker_replies reply, Task_manager::Actor) : _reply{reply} {}

    auto on_start(double) -> void {}
    auto on_stop() -> void {}

    auto handle_from_processor(const From_processor& message) -> void
    {
        std::visit(Inline_visitor{
            [this](const models::Design& d) {
                auto curve = models::Curve{.seq = d.seq, .drive = d.drive};
                const auto norm = std::tanh(d.drive);
                for (auto i = size_t{}; i < models::curve_points; ++i) {
                    const auto x = -1. + 2. * static_cast<double>(i) / static_cast<double>(models::curve_points - 1);
                    curve.table[i] = static_cast<float>(std::tanh(d.drive * x) / norm);
                }
                ++_designs;
                _reply.to_processor(curve);
            },
            [this](const models::Heartbeat& h) { _reply.to_processor(models::Echo{h.seq, h.frame}); },
        }, message);
    }

    auto handle_from_editor(const From_editor& message) -> void
    {
        std::visit(Inline_visitor{
            [this](const models::Ping& p) { _reply.to_editor(models::Pong{p.seq, p.sent_ns, _designs}); },
        }, message);
    }

private:

    Worker_replies _reply{};
    uint32_t _designs{};

};

} // namespace tiny::work

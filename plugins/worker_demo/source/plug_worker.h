#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <variant>

#include "tinyplug/tinyplug.h"

namespace tiny {

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

class Plug_worker {
public:

    using From_processor = std::variant<Tick>;
    using From_editor    = std::variant<Set_session>;
    using To_processor   = std::variant<Set_counter>;
    using To_editor      = std::variant<Session_path>;

    static constexpr auto inbound_capacity = size_t{64};
    static constexpr auto reply_capacity = size_t{16};
    static constexpr auto poll_interval = std::chrono::milliseconds{16};

    explicit Plug_worker(Worker_reply_actor<Plug_worker> reply, Task_manager::Actor tasks)
        : _reply{reply}, _tasks{tasks} {}

    auto on_start(double /*sample_rate*/) -> void {}
    auto on_stop() -> void {}

    auto handle_from_processor(const From_processor& m) -> void
    {
        std::visit(Inline_visitor{
            [this](const Tick&) {
                ++_count;
                _reply.to_processor(Set_counter{.count = _count});
            }
        }, m);
    }

    auto handle_from_editor(const From_editor& m) -> void
    {
        std::visit(Inline_visitor{
            [this](const Set_session& s) {
                auto path = Session_path{};
                const auto* src = s.uuid.data();
                std::copy_n(src, std::min(s.uuid.size(), path.path.size()), path.path.begin());
                _reply.to_editor(path);
            }
        }, m);
    }

private:

    Worker_reply_actor<Plug_worker> _reply{};
    Task_manager::Actor _tasks{};
    uint64_t _count{};

};

} // namespace tiny

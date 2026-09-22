#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <functional>
#include <variant>

#include "task_manager.hpp"

namespace tiny::work {

// The channel shape a plug-in declares as `models::Work` in `models/work.hpp`: the four typed
// message variants plus queue and polling tuning. Core-only, so it can be read without the
// user's `Worker` class. Alternatives must be trivially copyable (VST3 ships them over IMessage).
template<typename T>
concept Model = requires {
    typename T::From_processor;
    typename T::From_editor;
    typename T::To_processor;
    typename T::To_editor;
    { T::inbound_capacity } -> std::convertible_to<std::size_t>;
    { T::outbound_capacity } -> std::convertible_to<std::size_t>;
    { T::update_period } -> std::convertible_to<std::chrono::milliseconds>;
};

} // namespace tiny::work

namespace tiny {

// MARK: - actors

// Sends inbound messages from a single origin (processor or editor) into the
// worker. Constructed with a callable that pushes to the underlying queue
// (or forwards across an IPC boundary in the VST3 case). Default-constructed
// actor is a no-op (push returns false).
template <typename Msg>
class Worker_actor {
public:

    using Push_fn = std::function<bool(const Msg&)>;

    Worker_actor() = default;
    explicit Worker_actor(Push_fn fn) : _fn{std::move(fn)} {}

    auto push(const Msg& msg) const -> bool
    {
        return _fn ? _fn(msg) : false;
    }

private:

    Push_fn _fn{};

};

// Sends replies from the worker back to the processor and editor.
// Default-constructed → no-op. Unconstrained so `work::None` can name it while incomplete.
template <typename Work>
class Worker_reply_actor {
public:

    using To_processor = typename Work::To_processor;
    using To_editor = typename Work::To_editor;

    using To_processor_fn = std::function<bool(const To_processor&)>;
    using To_editor_fn = std::function<bool(const To_editor&)>;

    Worker_reply_actor() = default;
    Worker_reply_actor(To_processor_fn tp, To_editor_fn te)
        : _to_proc{std::move(tp)}, _to_edit{std::move(te)} {}

    auto to_processor(const To_processor& m) const -> bool
    {
        return _to_proc ? _to_proc(m) : false;
    }

    auto to_editor(const To_editor& m) const -> bool
    {
        return _to_edit ? _to_edit(m) : false;
    }

private:

    To_processor_fn _to_proc{};
    To_editor_fn _to_edit{};

};

} // namespace tiny

namespace tiny::work {

// What a plug-in without `models/work.hpp` + `worker.hpp` resolves to, in both roles: an empty
// channel model and a no-op worker. `TINY_HAS_WORKER` gates the plumbing out entirely.
struct None {
    using From_processor = std::monostate;
    using From_editor    = std::monostate;
    using To_processor   = std::monostate;
    using To_editor      = std::monostate;

    static constexpr auto inbound_capacity  = std::size_t{16};
    static constexpr auto outbound_capacity = std::size_t{16};
    static constexpr auto update_period = std::chrono::milliseconds{16};

    explicit None(Worker_reply_actor<None> = {}, Task_manager::Actor = Task_manager::Actor{nullptr}) {}

    auto on_start(double /*sample_rate*/) -> void {}
    auto on_stop() -> void {}
    auto handle_from_processor(const From_processor&) -> void {}
    auto handle_from_editor(const From_editor&) -> void {}
};

} // namespace tiny::work

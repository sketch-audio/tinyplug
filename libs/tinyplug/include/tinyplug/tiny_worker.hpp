#pragma once

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <functional>
#include <thread>
#include <type_traits>
#include <variant>

#include "lock_free_queue.hpp"
#include "task_manager.hpp"

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
// Default-constructed → no-op.
template <typename Worker>
class Worker_reply_actor {
public:

    using To_processor = typename Worker::Model::To_processor;
    using To_editor = typename Worker::Model::To_editor;

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

// MARK: - No_worker stub

// Default "no worker" type. The wrapper sees this when the plug-in has no
// `plug_worker.h`. All members are monostate / no-ops so the wrapper code
// compiles uniformly; `if constexpr (has_worker)` gates skip the runtime work.
struct No_worker {

    struct Model {
        using From_processor = std::monostate;
        using From_editor    = std::monostate;
        using To_processor   = std::monostate;
        using To_editor      = std::monostate;

        static constexpr auto inbound_capacity  = size_t{16};
        static constexpr auto outbound_capacity = size_t{16};
        static constexpr auto update_period = std::chrono::milliseconds{16};
    };

    using From_processor = Model::From_processor;
    using From_editor    = Model::From_editor;

    explicit No_worker(Worker_reply_actor<No_worker> = {}, Task_manager::Actor a = Task_manager::Actor{nullptr})
    {
        (void)a;
    }

    auto on_start(double /*sample_rate*/) -> void {}
    auto on_stop() -> void {}
    auto handle_from_processor(const From_processor&) -> void {}
    auto handle_from_editor(const From_editor&) -> void {}

};

} // namespace tiny

// MARK: - user worker discovery

// If the plug-in defines a `plug_worker.h` in its source directory, pull it
// in and use its `plugin::Worker` as the framework's `User_worker`. Otherwise
// `User_worker` aliases the stub above and `has_worker` is false.
//
// `TINY_HAS_WORKER` is the preprocessor counterpart — used in wrappers to
// `#if`-out worker member declarations entirely, so plug-ins without a
// `plug_worker.h` carry zero worker-related queues, threads, or storage.
// (The constexpr `has_worker` further down gates code inside templated
// helpers, where `if constexpr` properly discards.)
#if __has_include("worker.hpp")
    #include "worker.hpp"
    #define TINY_HAS_WORKER 1
    namespace tiny { using User_worker = plugin::Worker; }
#else
    #define TINY_HAS_WORKER 0
    namespace tiny { using User_worker = No_worker; }
#endif

namespace tiny {

inline constexpr bool has_worker = !std::is_same_v<User_worker, No_worker>;

// MARK: - convenience aliases

using Worker_processor_actor = Worker_actor<typename User_worker::Model::From_processor>;
using Worker_editor_actor    = Worker_actor<typename User_worker::Model::From_editor>;
using Worker_replies         = Worker_reply_actor<User_worker>;

// MARK: - concepts

// plugin::Processor opts in to worker-reply handling by defining
// `handle_worker_reply(const User_worker::Model::To_processor&)`.
template <typename P>
concept Receives_worker_reply_to_processor = requires (P p, const typename User_worker::Model::To_processor& r) {
    { p.handle_worker_reply(r) } -> std::same_as<void>;
};

// plugin::Editor opts in by defining
// `on_worker_reply(const User_worker::Model::To_editor&)`.
template <typename E>
concept Receives_worker_reply_to_editor = requires (E e, const typename User_worker::Model::To_editor& r) {
    { e.on_worker_reply(r) } -> std::same_as<void>;
};

// MARK: - helpers
//
// These are templates on purpose: inside a non-template member function,
// `if constexpr (false)` still performs name lookup on the discarded branch,
// so calling a method that may not exist on the user's class would not
// compile. Wrapping the guarded calls in function templates pushes the
// branches into dependent contexts where `if constexpr` properly discards
// them.

// Generic — calls `obj.bind_worker(a)` if the class has a matching overload,
// otherwise no-op. Works for both processor-side and editor-side actors.
template <typename T, typename A>
inline auto try_bind_worker(T& obj, const A& a) -> void
{
    if constexpr (requires { obj.bind_worker(a); }) {
        obj.bind_worker(a);
    }
}

template <typename P, typename Q>
inline auto try_drain_worker_to_processor(P& processor, Q& queue) -> void
{
    if constexpr (has_worker && Receives_worker_reply_to_processor<P>) {
        if constexpr (!std::is_same_v<typename User_worker::Model::To_processor, std::monostate>) {
            auto reply = typename User_worker::Model::To_processor{};
            while (queue.pop(reply)) {
                processor.handle_worker_reply(reply);
            }
        }
    }
}

template <typename E, typename Q>
inline auto try_drain_worker_to_editor(E& editor, Q& queue) -> void
{
    if constexpr (has_worker && Receives_worker_reply_to_editor<E>) {
        if constexpr (!std::is_same_v<typename User_worker::Model::To_editor, std::monostate>) {
            auto reply = typename User_worker::Model::To_editor{};
            while (queue.pop(reply)) {
                editor.on_worker_reply(reply);
            }
        }
    }
}

// MARK: - runner

// Owns the worker thread. Polls both inbound queues at the worker's
// `Model::update_period` and dispatches each message to its origin-specific handler.
// An optional `Post_cycle` callback runs on the worker thread at the end of
// each poll cycle — used (e.g. by VST3) to drain a worker → processor reply
// queue and forward over IPC without spawning a separate thread.
template <typename Worker>
class Worker_runner {
public:

    using From_processor = typename Worker::Model::From_processor;
    using From_editor    = typename Worker::Model::From_editor;

    static constexpr auto inbound_capacity = Worker::Model::inbound_capacity;

    using From_proc_queue = Lock_free_queue<From_processor, inbound_capacity, Queue_concurrency::spsc>;
    using From_edit_queue = Lock_free_queue<From_editor,    inbound_capacity, Queue_concurrency::spsc>;

    using Post_cycle = std::function<void()>;

    Worker_runner(Worker* worker, From_proc_queue* from_proc, From_edit_queue* from_edit)
        : _worker{worker}, _from_proc{from_proc}, _from_edit{from_edit} {}

    Worker_runner(const Worker_runner&) = delete;
    auto operator=(const Worker_runner&) -> Worker_runner& = delete;
    Worker_runner(Worker_runner&&) = delete;
    auto operator=(Worker_runner&&) -> Worker_runner& = delete;

    ~Worker_runner() { stop(); }

    auto set_post_cycle(Post_cycle fn) -> void { _post_cycle = std::move(fn); }

    auto start(double sample_rate) -> void
    {
        if constexpr (std::is_same_v<Worker, No_worker>) {
            return;
        }
        else {
            if (_running.exchange(true, std::memory_order_acq_rel)) return; // Already running.
            _thread = std::thread([this, sample_rate]() {
                _worker->on_start(sample_rate);
                while (_running.load(std::memory_order_acquire)) {
                    auto drained = false;

                    if constexpr (!std::is_same_v<From_processor, std::monostate>) {
                        auto m = From_processor{};
                        while (_from_proc->pop(m)) {
                            _worker->handle_from_processor(m);
                            drained = true;
                        }
                    }

                    if constexpr (!std::is_same_v<From_editor, std::monostate>) {
                        auto m = From_editor{};
                        while (_from_edit->pop(m)) {
                            _worker->handle_from_editor(m);
                            drained = true;
                        }
                    }

                    if (_post_cycle) _post_cycle();

                    // Optional periodic hook: a true wall-clock tick on the worker
                    // thread, independent of message flow. Opt-in — workers that
                    // don't define on_update() are unaffected.
                    if constexpr (requires { _worker->on_update(); }) {
                        _worker->on_update();
                    }

                    if (!drained) {
                        std::this_thread::sleep_for(Worker::Model::update_period);
                    }
                }
                _worker->on_stop();
            });
        }
    }

    auto stop() -> void
    {
        if constexpr (std::is_same_v<Worker, No_worker>) {
            return;
        }
        else {
            if (!_running.exchange(false, std::memory_order_acq_rel)) return;
            if (_thread.joinable()) _thread.join();
        }
    }

private:

    Worker* _worker{nullptr};
    From_proc_queue* _from_proc{nullptr};
    From_edit_queue* _from_edit{nullptr};
    Post_cycle _post_cycle{};
    std::atomic<bool> _running{false};
    std::thread _thread{};

};

} // namespace tiny

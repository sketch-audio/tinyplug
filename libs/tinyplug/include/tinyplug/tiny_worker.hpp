#pragma once

#include <atomic>
#include <concepts>
#include <functional>
#include <thread>
#include <type_traits>
#include <variant>

#include <tiny_core/lock_free_queue.hpp>
#include <tiny_core/tiny_work.hpp>

#include <tiny_models.hpp>

namespace tiny {

// MARK: - convenience aliases

using Worker_processor_actor = Worker_actor<User_work::From_processor>;
using Worker_editor_actor    = Worker_actor<User_work::From_editor>;
using Worker_replies         = Worker_reply_actor<User_work>;

// MARK: - concepts

// process::Processor opts in to worker-reply handling by defining
// `handle_worker_reply(const User_work::To_processor&)`.
template <typename P>
concept Receives_worker_reply_to_processor = requires (P p, const User_work::To_processor& r) {
    { p.handle_worker_reply(r) } -> std::same_as<void>;
};

// edit::Editor opts in by defining
// `on_worker_reply(const User_work::To_editor&)`.
template <typename E>
concept Receives_worker_reply_to_editor = requires (E e, const User_work::To_editor& r) {
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
    if constexpr (has_work && Receives_worker_reply_to_processor<P>) {
        if constexpr (!std::is_same_v<User_work::To_processor, std::monostate>) {
            auto reply = User_work::To_processor{};
            while (queue.pop(reply)) {
                processor.handle_worker_reply(reply);
            }
        }
    }
}

template <typename E, typename Q>
inline auto try_drain_worker_to_editor(E& editor, Q& queue) -> void
{
    if constexpr (has_work && Receives_worker_reply_to_editor<E>) {
        if constexpr (!std::is_same_v<User_work::To_editor, std::monostate>) {
            auto reply = User_work::To_editor{};
            while (queue.pop(reply)) {
                editor.on_worker_reply(reply);
            }
        }
    }
}

// MARK: - runner

// Owns the worker thread. Polls both inbound queues at the work model's
// `update_period` and dispatches each message to its origin-specific handler.
// An optional `Post_cycle` callback runs on the worker thread at the end of
// each poll cycle — used (e.g. by VST3) to drain a worker → processor reply
// queue and forward over IPC without spawning a separate thread.
template <typename Worker>
class Worker_runner {
public:

    using From_processor = User_work::From_processor;
    using From_editor    = User_work::From_editor;

    static constexpr auto inbound_capacity = User_work::inbound_capacity;

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
        if constexpr (std::is_same_v<Worker, work::None>) {
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
                        std::this_thread::sleep_for(User_work::update_period);
                    }
                }
                _worker->on_stop();
            });
        }
    }

    auto stop() -> void
    {
        if constexpr (std::is_same_v<Worker, work::None>) {
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

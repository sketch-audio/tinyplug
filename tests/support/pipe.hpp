#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <vector>

namespace tiny::link {

using Sink = std::function<void(std::span<const std::byte> bytes, uint32_t tag)>;

// One direction of the editor <-> processor boundary.
//
//   Immediate  — the two halves share memory and the call lands now (CLAP, AUv2, AUv3).
//   Queued     — the payload is copied and delivered when the host gets around to it
//                (VST3 IMessage, AAX Direct Data). `pump()` is the host.
//
// Counters exist so the tests can report what the wire actually costs.
class Pipe {
public:

    explicit Pipe(bool immediate, size_t capacity = 32)
        : _immediate{immediate}, _capacity{capacity} {}

    auto set_sink(Sink sink) -> void { _sink = std::move(sink); }

    auto send(std::span<const std::byte> bytes, uint32_t tag) -> bool
    {
        _sent_msgs += 1;
        _sent_bytes += bytes.size();

        if (_immediate) {
            if (_sink) _sink(bytes, tag);
            return true;
        }

        auto lock = std::lock_guard{_m};
        if (_queue.size() >= _capacity) { _dropped += 1; return false; }
        _queue.push_back(Msg{{bytes.begin(), bytes.end()}, tag});
        return true;
    }

    // [host] Deliver up to `max` queued messages. Returns how many landed.
    auto pump(size_t max = static_cast<size_t>(-1)) -> size_t
    {
        auto batch = std::vector<Msg>{};
        {
            auto lock = std::lock_guard{_m};
            const auto n = std::min(max, _queue.size());
            batch.assign(std::make_move_iterator(_queue.begin()),
                         std::make_move_iterator(_queue.begin() + static_cast<long>(n)));
            _queue.erase(_queue.begin(), _queue.begin() + static_cast<long>(n));
        }
        for (const auto& m : batch) if (_sink) _sink(m.bytes, m.tag);
        return batch.size();
    }

    auto pending() const -> size_t { auto l = std::lock_guard{_m}; return _queue.size(); }
    auto sent_bytes() const -> size_t { return _sent_bytes; }
    auto sent_msgs() const -> size_t { return _sent_msgs; }
    auto dropped() const -> size_t { return _dropped; }
    auto reset_counters() -> void { _sent_bytes = 0; _sent_msgs = 0; _dropped = 0; }

private:

    struct Msg {
        std::vector<std::byte> bytes{};
        uint32_t tag{};
    };

    bool _immediate{};
    size_t _capacity{};
    Sink _sink{};

    mutable std::mutex _m{};
    std::vector<Msg> _queue{};

    size_t _sent_bytes{};
    size_t _sent_msgs{};
    size_t _dropped{};

};

} // namespace tiny::link

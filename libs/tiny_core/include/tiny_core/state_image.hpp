#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <utility>

namespace tiny {

/*
    Persisted state for a format whose host may save and load from any thread: AAX chunks,
    AUv2 ClassInfo, AUv3 fullState. The state itself lives on main (editor, undo, document), so
    main keeps an image of it and any thread serves that. A load from off main becomes the image
    at once and waits here for main to apply it.

    Main rebuilds when `due`: dirty (a parameter or a load moved) and `min_interval` since the
    last rebuild, or `poll_interval` regardless. The poll is what catches editor state and the
    document, which have no change signal. `Image` must be cheap to copy: a shared pointer or a
    retained handle.
*/
template<typename Image>
class State_image {
public:

    static constexpr auto min_interval = std::chrono::milliseconds{250}; // So automation cannot rebuild every tick.
    static constexpr auto poll_interval = std::chrono::seconds{1};

    // [any] The state as a save should see it.
    auto get() const -> Image
    {
        const auto lock = std::lock_guard{_mutex};
        return _image;
    }

    // [main] A rebuild. Refused while a load waits: the load is the state until main applies it.
    auto publish(Image image) -> bool
    {
        const auto lock = std::lock_guard{_mutex};
        if (_pending) return false;
        _image = std::move(image);
        _dirty.store(false, std::memory_order_relaxed);
        _built = std::chrono::steady_clock::now();
        return true;
    }

    // [any] A load from off main.
    auto post_load(Image image) -> void
    {
        const auto lock = std::lock_guard{_mutex};
        _pending = image;
        _image = std::move(image);
    }

    // [main] The waiting load, to apply now.
    auto take_load() -> std::optional<Image>
    {
        const auto lock = std::lock_guard{_mutex};
        auto out = std::exchange(_pending, std::nullopt);
        if (out) _dirty.store(true, std::memory_order_relaxed);
        return out;
    }

    // [any] A later request supersedes a load still waiting.
    auto drop_load() -> void
    {
        const auto lock = std::lock_guard{_mutex};
        _pending.reset();
    }

    // [any] Something the image holds has changed.
    auto mark_dirty() -> void { _dirty.store(true, std::memory_order_relaxed); }

    // [main]
    auto due() const -> bool
    {
        const auto lock = std::lock_guard{_mutex};
        const auto since = std::chrono::steady_clock::now() - _built;
        return !_pending && since >= min_interval && (_dirty.load(std::memory_order_relaxed) || since >= poll_interval);
    }

private:

    mutable std::mutex _mutex{};
    Image _image{};
    std::optional<Image> _pending{};
    std::atomic<bool> _dirty{true};
    std::chrono::steady_clock::time_point _built{};

};

} // namespace tiny

#pragma once

#include <mutex>
#include <optional>
#include <utility>

namespace tiny {

// A load a host made off main (AAX chunks, AUv2 ClassInfo, AUv3 fullState), waiting for main to apply
// it: the editor, undo and document are main's. Until then it is the state, so a save from any thread
// answers with it, and a save straight after a load returns what was loaded. Saves otherwise build on
// the calling thread from what is safe to read anywhere. `Image` must be cheap to copy: a shared
// pointer or a retained handle.
template<typename Image>
class Pending_load {
public:

    // [any] A load from off main.
    auto post(Image image) -> void
    {
        const auto lock = std::lock_guard{_mutex};
        _pending = std::move(image);
    }

    // [any] The waiting load, for a save to answer with.
    auto peek() const -> std::optional<Image>
    {
        const auto lock = std::lock_guard{_mutex};
        return _pending;
    }

    // [main] The waiting load, to apply now.
    auto take() -> std::optional<Image>
    {
        const auto lock = std::lock_guard{_mutex};
        return std::exchange(_pending, std::nullopt);
    }

    // [any] A later request supersedes a load still waiting.
    auto drop() -> void
    {
        const auto lock = std::lock_guard{_mutex};
        _pending.reset();
    }

private:

    mutable std::mutex _mutex{};
    std::optional<Image> _pending{};

};

} // namespace tiny

#pragma once

#include <functional>
#include <memory>
#include <optional>

#include "tiny_view.h"

namespace tiny {

// MARK: - Interface

class Gesture_recognizer {
public:
    virtual ~Gesture_recognizer() = default;
    virtual auto set_frame(const Frame& frame) -> void = 0;
    virtual auto process_events(Event_list& events) -> void = 0;
};

template<typename T>
using On_started = std::function<void(const T&)>;

template<typename T>
using On_updated = std::function<void(const T&)>;

template<typename T>
using On_ended = std::function<void(const T&)>;

using On_cancelled = std::function<void()>;

template<typename T>
struct Gesture_callbacks {
    On_started<T> on_started{[](auto) {}};
    On_updated<T> on_updated{[](auto) {}};
    On_ended<T> on_ended{[](auto) {}};
    On_cancelled on_cancelled{[]() {}};
};

// MARK: - Over

class Over_recognizer : public Gesture_recognizer {
public:
    
    struct Info {
        Coords pos{};
        bool over{};
    };

    explicit Over_recognizer(const Gesture_callbacks<Info>& callbacks)
        : _callbacks{callbacks} {}

    auto set_frame(const Frame& frame) -> void override;
    auto process_events(Event_list& events) -> void override;

private:
    
    Gesture_callbacks<Info> _callbacks{};
    Frame _frame{};
    bool _over{};

    auto resolve_events(Coords pos, bool was_over, bool now_over) -> void;

};

using Over_info = Over_recognizer::Info;

// MARK: - Down

class Down_recognizer : public Gesture_recognizer {
public:

    struct Info {
        Coords pos{};
        bool down{};
    };

    explicit Down_recognizer(const Gesture_callbacks<Info>& callbacks)
        : _callbacks{callbacks} {}

    auto set_frame(const Frame& frame) -> void override;
    auto process_events(Event_list& events) -> void override;

private:

    Gesture_callbacks<Info> _callbacks{};
    Frame _frame{};
    bool _down{};

};

using Down_info = Down_recognizer::Info;

// MARK: - Dwell

class Dwell_recognizer : public Gesture_recognizer {
public:

    struct Info {
        Coords pos{};
        bool dwelling{};
    };

    explicit Dwell_recognizer(const Gesture_callbacks<Info>& callbacks)
        : _callbacks{callbacks} {}

    auto set_frame(const Frame& frame) -> void override;
    auto process_events(Event_list& events) -> void override;


private:

    static constexpr auto dwell_ms = 2000;

    Gesture_callbacks<Info> _callbacks{};
    Frame _frame{};
    bool _down{}; // No dwell when down.

    std::optional<Steady_time> _over_t{};
    Coords _pos{};
    bool _dwelling{};

    auto end_dwell(Coords pos) -> void;

};

using Dwell_info = Dwell_recognizer::Info;

// MARK: - Click

class Click_recognizer : public Gesture_recognizer {
public:

    struct Desc {
        Pointer_button button{};
        uint32_t count{1};
        bool greedy{};
    };

    struct Info {
        Coords pos{};
    };

    explicit Click_recognizer(Gesture_callbacks<Info> callbacks, const Desc& desc)
        : _callbacks{callbacks}, _desc{desc} {}

    auto set_frame(const Frame& frame) -> void override;
    auto process_events(Event_list& events) -> void override;


private:

    Gesture_callbacks<Info> _callbacks{};
    Desc _desc{};
    Frame _frame{};

};

using Click_info = Click_recognizer::Info;

// MARK: - Drag

class Drag_recognizer : public Gesture_recognizer {
public:

    struct Info {
        Coords fpos{};
        Coords tpos{};
    };

    explicit Drag_recognizer(const Gesture_callbacks<Info>& callbacks, bool greedy = false)
        : _callbacks{callbacks}, _greedy{greedy} {}

    auto set_frame(const Frame& frame) -> void override;
    auto process_events(Event_list& events) -> void override;

private:

    Gesture_callbacks<Info> _callbacks{};
    bool _greedy{};

    Frame _frame{};

    std::optional<Coords> _fpos{};
    std::optional<uintptr_t> _tag{};
    bool _initiated{};

    auto reset() -> void;
    
};

using Drag_info = Drag_recognizer::Info;

// MARK: - Factory

// Factory to create recognizers with deduced callback types.
template<typename Recognizer, typename... Args>
auto make_recognizer(Gesture_callbacks<typename Recognizer::Info> callbacks, Args&&... args) -> std::unique_ptr<Recognizer>
{
    return std::make_unique<Recognizer>(std::move(callbacks), std::forward<Args>(args)...);
}

} // namespace tiny
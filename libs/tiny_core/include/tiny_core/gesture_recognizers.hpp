#pragma once

#include <functional>
#include <memory>
#include <optional>

#include "tiny_input.hpp"

namespace tiny::gesture {

// MARK: - Interface

class Recognizer {
public:
    virtual ~Recognizer() = default;
    virtual auto set_frame(const Frame& frame) -> void = 0;
    virtual auto process_events(Event_list& events) -> void = 0;
};

template<typename T>
struct Callbacks {
    std::function<void(const T&)> on_started{[](auto) {}};
    std::function<void(const T&)> on_updated{[](auto) {}};
    std::function<void(const T&)> on_ended{[](auto) {}};
    std::function<void()> on_cancelled{[]() {}};
};

// MARK: - Over

class Over : public Recognizer {
public:
    
    struct Info {
        Coords pos{};
        bool over{};
    };

    explicit Over(const Callbacks<Info>& callbacks)
        : _callbacks{callbacks} {}

    auto set_frame(const Frame& frame) -> void override;
    auto process_events(Event_list& events) -> void override;

private:
    
    Callbacks<Info> _callbacks{};
    Frame _frame{};
    bool _over{};
    std::optional<uintptr_t> _tag{};

    auto resolve_events(Coords pos, bool was_over, bool now_over, uintptr_t tag) -> void;

};

// MARK: - Down

class Down : public Recognizer {
public:

    struct Info {
        Coords pos{};
        bool down{};
    };

    explicit Down(const Callbacks<Info>& callbacks)
        : _callbacks{callbacks} {}

    auto set_frame(const Frame& frame) -> void override;
    auto process_events(Event_list& events) -> void override;

private:

    Callbacks<Info> _callbacks{};
    Frame _frame{};
    bool _down{};
    std::optional<uintptr_t> _tag{};

};

// MARK: - Dwell

class Dwell : public Recognizer {
public:

    struct Info {
        Coords pos{};
        bool dwelling{};
    };

    explicit Dwell(const Callbacks<Info>& callbacks)
        : _callbacks{callbacks} {}

    auto set_frame(const Frame& frame) -> void override;
    auto process_events(Event_list& events) -> void override;


private:

    static constexpr auto dwell_ms = 2000;

    Callbacks<Info> _callbacks{};
    Frame _frame{};

    std::optional<Steady_time> _over_t{};
    Coords _pos{};
    bool _dwelling{};

    auto end_dwell(Coords pos) -> void;

};

// MARK: - Click

class Click : public Recognizer {
public:

    struct Desc {
        Pointer::Button button{};
        uint32_t count{1};
        bool greedy{};
    };

    struct Info {
        Coords pos{};
    };

    explicit Click(Callbacks<Info> callbacks, const Desc& desc)
        : _callbacks{callbacks}, _desc{desc} {}

    auto set_frame(const Frame& frame) -> void override;
    auto process_events(Event_list& events) -> void override;


private:

    Callbacks<Info> _callbacks{};
    Desc _desc{};
    Frame _frame{};

};

// MARK: - Drag

class Drag : public Recognizer {
public:

    struct Info {
        Coords fpos{};
        Coords tpos{};
    };

    // `start_slop` is the pointer travel, in points, a press must exceed before the
    // drag begins. 0 (the default) keeps the pixel-0 tracking drag-primary controls
    // want; click-primary controls set it so a wobble during a click is not a drag.
    explicit Drag(const Callbacks<Info>& callbacks, bool greedy = false,
                             bool cancels_on_frame_change = true, double start_slop = 0.)
        : _callbacks{callbacks}, _greedy{greedy}, _cancels_on_frame_change{cancels_on_frame_change},
          _start_slop{start_slop} {}

    auto set_frame(const Frame& frame) -> void override;
    auto process_events(Event_list& events) -> void override;

private:

    Callbacks<Info> _callbacks{};
    bool _greedy{};
    bool _cancels_on_frame_change{};
    double _start_slop{};

    Frame _frame{};

    std::optional<Coords> _fpos{};
    std::optional<uintptr_t> _tag{};
    bool _initiated{};

    auto past_slop(Coords fpos, Coords tpos) const -> bool;
    auto reset() -> void;
    
};

// MARK: - Factory

// Factory to create recognizers with deduced callback types.
template<typename R, typename... Args>
auto make(Callbacks<typename R::Info> callbacks, Args&&... args) -> std::unique_ptr<R>
{
    return std::make_unique<R>(std::move(callbacks), std::forward<Args>(args)...);
}

} // namespace tiny::gesture
#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <variant>

#include "tiny_input.hpp" // Rect_size

namespace tiny {

// MARK: - Fixed_string

// A string with fixed capacity, trivially copyable, so it can live in view state or a document.
// Longer text is cut at the capacity (whole UTF-8 sequences may not survive the cut).
template<std::size_t N>
struct Fixed_string {
    std::array<char, N> chars{};

    Fixed_string() = default;
    Fixed_string(std::string_view text) { assign(text); }
    Fixed_string(const char* text) { assign(text); }
    auto operator=(std::string_view text) -> Fixed_string& { assign(text); return *this; }
    auto operator=(const char* text) -> Fixed_string& { assign(text); return *this; }

    auto assign(std::string_view text) -> void
    {
        chars.fill('\0');
        std::copy_n(text.begin(), std::min(text.size(), N - 1), chars.begin());
    }

    auto view() const -> std::string_view { return {chars.data(), std::char_traits<char>::length(chars.data())}; }
    auto empty() const -> bool { return chars[0] == '\0'; }
    operator std::string_view() const { return view(); }

    auto operator==(const Fixed_string&) const -> bool = default;
};

// MARK: - Sizing

// The editor's window: where it opens, its limits, and whether a resize the editor makes is an undo step.
// The framework stores the size with the session and clamps in each format's own size check.
struct Sizing {
    Rect_size preferred{};
    Rect_size min{1, 1};
    std::optional<Rect_size> max{};
    bool undoable{};

    constexpr auto clamp(Rect_size size) const -> Rect_size
    {
        size.w = std::max(size.w, min.w);
        size.h = std::max(size.h, min.h);
        if (max) {
            size.w = std::min(size.w, max->w);
            size.h = std::min(size.h, max->h);
        }
        return size;
    }
};

// MARK: - Load_info

// The editor state saved by builds before the view model: string keys, written by the author's
// `save_state`. Read forever, since shipped sessions hold it; reaches the author as Load_info::legacy.
using State_item = std::variant<bool, int32_t, double, std::string>;
using State_map = std::unordered_map<std::string, State_item>;

// What a host load was, reported to the view model's `on_host_load`. The framework says what
// happened; what it means (a displayed preset name, say) is the plug-in's business.
struct Load_info {
    enum class Kind : std::uint8_t { Session, Preset };
    Kind kind{Kind::Session};
    std::string_view name{};   // A preset's name, when the load path knows it.
    std::string_view id{};     // A preset's id, when its file carries one.
    const State_map* legacy{}; // The author's keys from a session or preset saved before the view model.
    bool view_restored{};      // The data carried a view record.
};

} // namespace tiny

namespace tiny::view {

// MARK: - Model

// The view model, `models::View` in models/view.hpp: what the editor wants back with the session, in
// two parts. `Undoable` follows undo ("what I did": an A/B selection, a displayed preset name);
// `Restored` comes back with the session but undo never touches it ("where I was looking": a tab,
// zoom). Both trivially copyable; each may declare a state record's `save` / `load` pair for versioning.
// Optional: `static auto on_host_load(View&, std::span<const double> knob_params, const Load_info&) -> void`,
// called on main inside every host load's undo step, window open or closed.
template<typename T>
concept Model = requires {
    typename T::Undoable;
    typename T::Restored;
    requires std::is_trivially_copyable_v<typename T::Undoable>;
    requires std::is_trivially_copyable_v<typename T::Restored>;
    requires std::is_default_constructible_v<T>;
    requires std::is_trivially_copyable_v<T>;
};

template<typename T>
concept Has_load_hook = requires(T& view, std::span<const double> params, const Load_info& info) {
    { T::on_host_load(view, params, info) } -> std::same_as<void>;
};

// Fallback when the plug-in has no `models/view.hpp`.
struct None {
    struct Undoable {};
    struct Restored {};
    Undoable undoable{};
    Restored restored{};
};

} // namespace tiny::view

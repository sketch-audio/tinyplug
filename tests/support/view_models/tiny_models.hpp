// Stand-in for the header configure_models() generates, with a view model and nothing else, for
// `Edit_store` (edit_store_test).
#pragma once

#include <tiny_core/tiny_core.hpp>

#define TINY_HAS_PARAMS 0
#define TINY_HAS_METERS 0
#define TINY_HAS_BLOCKS 0
#define TINY_HAS_STATE 0
#define TINY_HAS_WORK 0
#define TINY_HAS_VIEW 1
#define TINY_HAS_NOTES_IN 0
#define TINY_HAS_NOTES_OUT 0
#define TINY_HAS_NOTE_EXPRESSION 0

namespace tiny::models {

// What a client's view looks like: a preset name and an A/B selection follow undo; a tab doesn't.
struct Test_view {
    struct Undoable {
        Fixed_string<32> preset_name{};
        std::uint8_t b_selected{};
        std::uint8_t loads_seen{};
    } undoable{};
    struct Restored {
        std::uint8_t tab{};
    } restored{};

    static auto on_host_load(Test_view& v, std::span<const double>, const Load_info& info) -> void
    {
        if (info.kind == Load_info::Kind::Preset) v.undoable.preset_name = info.name;
        else if (info.legacy) {
            if (const auto it = info.legacy->find("preset-name"); it != info.legacy->end()) {
                if (const auto* name = std::get_if<std::string>(&it->second)) v.undoable.preset_name = *name;
            }
        }
        ++v.undoable.loads_seen;
    }
};

struct Resolved {
    using Params = params::None;
    using Meters = meters::None;
    using Blocks = blocks::None;
    using State = state::None;
    using Work = work::None;
    using View = Test_view;
};

} // namespace tiny::models

namespace tiny {

using User_params = params::Infos<models::Resolved::Params>;
using User_meters = meters::Infos<models::Resolved::Meters>;
using User_blocks = blocks::Infos<models::Resolved::Blocks>;
using User_state = models::Resolved::State;
using User_work = models::Resolved::Work;
using User_view = models::Resolved::View;

inline constexpr bool has_params = false;
inline constexpr bool has_meters = false;
inline constexpr bool has_blocks = false;
inline constexpr bool has_state = false;
inline constexpr bool has_work = false;
inline constexpr bool has_view = true;
inline constexpr bool has_notes_in = false;
inline constexpr bool has_notes_out = false;

} // namespace tiny

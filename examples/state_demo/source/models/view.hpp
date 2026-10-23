#pragma once

#include <tiny_core/tiny_core.hpp>

namespace tiny::models {

// What the editor wants back with the session. The current preset's name follows undo: undoing a
// preset load, from the editor's buttons or the host's menu, brings back the name from before it.
struct View {
    struct Undoable {
        Fixed_string<64> preset_name{};
    } undoable{};
    struct Restored {} restored{};

    // Every host load, window open or closed, inside the load's undo step. A session restores the name
    // it saved; a preset names itself.
    static auto on_host_load(View& v, std::span<const double>, const Load_info& info) -> void
    {
        if (info.kind == Load_info::Kind::Preset) {
            v.undoable.preset_name = info.name;
        }
        else if (info.legacy) { // A session saved before the view model kept the name in the editor map.
            if (const auto it = info.legacy->find("preset-name"); it != info.legacy->end()) {
                if (const auto* name = std::get_if<std::string>(&it->second)) v.undoable.preset_name = *name;
            }
        }
    }
};
static_assert(view::Model<View>);

} // namespace tiny::models

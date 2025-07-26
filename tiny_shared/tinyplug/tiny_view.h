#pragma once

#include <span>

#include "../platform/graphics_delegate.h"

namespace tiny {

// The current state of the user's interaction(s).
struct Ui_state {/*TODO*/};

// The current parameter & export values.
struct Params_state {
    std::span<const double> params{};
    std::span<const double> exports{};
};

// A place to send your control's actions.
struct Action_receiver {/*TODO*/};

// A context in which to draw.
using Draw_context = Graphics_delegate::Draw_context;

// 
struct View_context {
    Ui_state ui_state{};
    Params_state params_state{};
    Action_receiver action_receiver{};
    Draw_context draw_context{};
};

}
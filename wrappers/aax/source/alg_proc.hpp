#pragma once

#include "alg_context.hpp"

namespace tiny::aax {

// The real-time algorithm entrypoints. One per stem format, because the channel
// count is a compile-time constant inside the render loop and each stem format gets
// its own ProcessProc registration in Describe.
void AAX_CALLBACK alg_render_mono(Alg_context* const instances_begin[], const void* instances_end);
void AAX_CALLBACK alg_render_stereo(Alg_context* const instances_begin[], const void* instances_end);

// Instance lifecycle, called in the algorithm's memory space: constructs and
// destroys Alg_state (including the user's processor) off the real-time thread.
int32_t AAX_CALLBACK alg_init(const Alg_context* context, AAX_EComponentInstanceInitAction action);

} // namespace tiny::aax

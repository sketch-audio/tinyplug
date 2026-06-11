#pragma once

// Umbrella header for the platform library. Pulls in the format-facing platform
// surface: compile-time platform detection, the native view + its delegate,
// dialogs, and paths.
//
// `window_context.hpp` is intentionally excluded — it is an internal
// implementation header (and the only one that pulls in Skia). Platform sources
// include it directly; consumers outside the platform library should not need it.

#include "../tinyplug/tiny_platform.hpp"
#include "view_delegate.hpp"
#include "platform_view.hpp"
#include "platform_dialogs.hpp"
#include "platform_paths.hpp"

#pragma once

// Umbrella header for the platform library. Pulls in the format-facing platform
// surface: compile-time platform detection, the native view + its delegate,
// dialogs, and paths.
//
// `window_context.hpp` is intentionally excluded — it is an internal
// implementation header (and the only one that pulls in Skia). Platform sources
// include it directly; consumers outside the platform library should not need it.

#include <tinyplug/platform_defs.hpp>
#include <tiny_platform/view_delegate.hpp>
#include <tiny_platform/platform_view.hpp>
#include <tiny_platform/platform_dialogs.hpp>
#include <tiny_platform/platform_paths.hpp>

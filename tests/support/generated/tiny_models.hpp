// Stand-in for the header configure_models() generates, so tests can include tinyplug's
// interface headers without a plug-in. No models: every optional one resolves to None.
#pragma once

#include <tiny_core/tiny_core.hpp>

#define TINY_HAS_PARAMS 0
#define TINY_HAS_METERS 0
#define TINY_HAS_BLOCKS 0
#define TINY_HAS_STATE 0
#define TINY_HAS_WORK 0

namespace tiny::models {

struct Resolved {
    using Params = params::None;
    using Meters = meters::None;
    using Blocks = blocks::None;
    using State = state::None;
    using Work = work::None;
};

} // namespace tiny::models

namespace tiny {

using User_params = params::Infos<models::Resolved::Params>;
using User_meters = meters::Infos<models::Resolved::Meters>;
using User_blocks = blocks::Infos<models::Resolved::Blocks>;
using User_state = models::Resolved::State;
using User_work = models::Resolved::Work;

inline constexpr bool has_params = false;
inline constexpr bool has_meters = false;
inline constexpr bool has_blocks = false;
inline constexpr bool has_state = false;
inline constexpr bool has_work = false;

} // namespace tiny

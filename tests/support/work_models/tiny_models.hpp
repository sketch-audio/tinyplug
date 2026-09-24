// Stand-in for the generated <tiny_models.hpp> with only a work model, for worker_runner_test.
#pragma once

#include <chrono>
#include <cstdint>
#include <variant>

#include <tiny_core/tiny_core.hpp>

#define TINY_HAS_PARAMS 0
#define TINY_HAS_METERS 0
#define TINY_HAS_BLOCKS 0
#define TINY_HAS_STATE 0
#define TINY_HAS_WORK 1
#define TINY_HAS_NOTES_IN 0
#define TINY_HAS_NOTES_OUT 0
#define TINY_HAS_NOTE_EXPRESSION 0

namespace tiny::models {

struct Test_work {
    struct Job { uint32_t seq{}; };
    struct Request { uint32_t seq{}; };
    struct Done { uint32_t seq{}; };
    struct Reply { uint32_t seq{}; };

    using From_processor = std::variant<Job>;
    using From_editor = std::variant<Request>;
    using To_processor = std::variant<Done>;
    using To_editor = std::variant<Reply>;

    static constexpr auto inbound_capacity = std::size_t{64};
    static constexpr auto outbound_capacity = std::size_t{64};
    static constexpr auto update_period = std::chrono::milliseconds{1};
};
static_assert(work::Model<Test_work>);

struct Resolved {
    using Params = params::None;
    using Meters = meters::None;
    using Blocks = blocks::None;
    using State = state::None;
    using Work = Test_work;
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
inline constexpr bool has_work = true;
inline constexpr bool has_notes_in = false;
inline constexpr bool has_notes_out = false;
inline constexpr bool has_note_expression = false;

} // namespace tiny

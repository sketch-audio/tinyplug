#pragma once

#include <array>
#include <cstdint>

#include "tiny_params.h"
#include "tiny_utils.h"

namespace tiny {

static constexpr auto EXPORT_OFFSET = int32_t{1 << 30};

template<Some_param_model User_model>
struct Exports {

    static constexpr auto num_exports = to_underlying(User_model::Export_id::num_exports);

    using Value_arr = std::array<double, num_exports>;

};

}
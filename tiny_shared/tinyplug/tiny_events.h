#pragma once

#include <variant>

namespace tiny {

struct Set_param {
    uint32_t id{};
    double value{};
};

struct Ramp_param {
    uint32_t id{};
    double target{};
    int32_t dur_samples{};
};

using Event = std::variant<Set_param, Ramp_param>;

struct Tagged_event {
    int32_t offset{};
    Event event{};
};

}
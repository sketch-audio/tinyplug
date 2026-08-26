#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace tiny::process {

struct Config {
    double sr{48000.};
    std::span<const double> params{};
};

struct Event {};

} // namespace tiny::process
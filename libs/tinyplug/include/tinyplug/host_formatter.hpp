#pragma once

#include <optional>
#include <string>

#include "tiny_params.hpp"

namespace tiny {

struct Host_formatter {
    // Parameter semantics.
    using Semantics = params::Semantics;

    // Value to string.
    static auto to_string(double host_value, const Semantics::Any& semantics) -> std::string;

    // String to value.
    static auto to_value(const std::string& string, const Semantics::Any& semantics) -> std::optional<double>;
    
};

} // namespace tiny;
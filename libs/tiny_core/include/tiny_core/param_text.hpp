#pragma once

#include <optional>
#include <string>

#include "tiny_params.hpp"

namespace tiny::params {

// A parameter's text, from its `display` (or a List's items): what every host and the editor show, so they agree.
// Both directions are plain space.
auto to_text(double plain_value, const Semantics::Any& semantics) -> std::string;
auto from_text(const std::string& text, const Semantics::Any& semantics) -> std::optional<double>;

} // namespace tiny::params

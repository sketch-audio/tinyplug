#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace tiny::platform {

struct Paths {

    struct Subpath {
        std::string manufacturer{};
        std::string product{};
        std::string app_group_id{}; // When set on iOS, uses App Group container.
    };

    static auto format_readable(const std::string& bundle_id) -> std::filesystem::path;

    // A folder every instance can write, per manufacturer and product (an App Group's on iOS).
    static auto shared_writable(const Subpath&) -> std::filesystem::path;

};

} // namespace tiny::platform
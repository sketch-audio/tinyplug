#include <tiny_platform/platform_paths.hpp>

#include <system_error>

namespace tiny {

// In-memory for now (MEMFS): an editor's prefs and user presets last the page's lifetime. IDBFS / OPFS mounted
// here is the persistent follow-up.
auto Platform_paths::format_readable(const std::string& /*bundle_id*/) -> std::filesystem::path
{
    return "/tinyplug/resources";
}

auto Platform_paths::shared_writable(const Subpath& subpath) -> std::filesystem::path
{
    const auto path = std::filesystem::path{"/tinyplug/user"} / subpath.manufacturer / subpath.product;
    auto ec = std::error_code{};
    std::filesystem::create_directories(path, ec);
    return path;
}

} // namespace tiny

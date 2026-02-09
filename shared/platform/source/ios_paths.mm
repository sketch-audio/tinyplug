#include "../platform_paths.hpp"

#include <Foundation/Foundation.h>

namespace tiny {

auto Platform_paths::format_readable(const std::string& /*bundle_id*/) -> std::filesystem::path
{
    // On iOS we have our own bundle.
    auto bundle = [NSBundle mainBundle];
    auto resources = [bundle resourcePath];
    if (!resources) return {};
    
    const auto path = std::filesystem::path{[resources UTF8String]};
    return path;
}

auto Platform_paths::shared_writable(const Subpath& subpath) -> std::filesystem::path
{
    auto paths = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
    auto app_support = [paths firstObject];
    if (!app_support) return {};
    
    const auto path = std::filesystem::path{[app_support UTF8String]} / subpath.manufacturer / subpath.product;
    return path;
}

} // namespace tiny

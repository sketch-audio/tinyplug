#include <tiny_platform/platform_paths.hpp>

#include <CoreFoundation/CoreFoundation.h>
#include <Foundation/Foundation.h>

#include "tinyplug/tinyplug.hpp"

#include <array>
#include <climits>

namespace tiny {

auto Platform_paths::format_readable(const std::string& bundle_id) -> std::filesystem::path
{
    // Look up by bundle identifier. Succeeds in both standlone and extension.
    const auto bundle_str = CFStringCreateWithCString(kCFAllocatorDefault, bundle_id.c_str(), kCFStringEncodingUTF8);
    const auto release_bundle_str = Deferred{[&]() { if (bundle_str) CFRelease(bundle_str); }};

    const auto bundle = CFBundleGetBundleWithIdentifier(bundle_str);
    if (!bundle) return {};

    const auto resources_url = CFBundleCopyResourcesDirectoryURL(bundle);
    const auto release_resources_url = Deferred{[&]() { if (resources_url) CFRelease(resources_url); }};

    auto buffer = std::array<char, PATH_MAX>{};
    auto data = reinterpret_cast<uint8_t*>(buffer.data());
    auto size = static_cast<CFIndex>(buffer.size());
    const auto result = CFURLGetFileSystemRepresentation(resources_url, true, data, size);
    if (!result) return {};

    const auto bundle_path = std::filesystem::path{buffer.data()};
    return bundle_path;
}

auto Platform_paths::shared_writable(const Subpath& subpath) -> std::filesystem::path
{
    if (!subpath.app_group_id.empty()) {
        NSString* group = [NSString stringWithUTF8String:subpath.app_group_id.c_str()];
        NSURL* container = [[NSFileManager defaultManager]
            containerURLForSecurityApplicationGroupIdentifier:group];
        if (container) {
            return std::filesystem::path{[container.path UTF8String]}
                / subpath.manufacturer / subpath.product;
        }
    }

    auto paths = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
    auto app_support = [paths firstObject];
    if (!app_support) return {};
    
    const auto path = std::filesystem::path{[app_support UTF8String]} / subpath.manufacturer / subpath.product;
    return path;
}

} // namespace tiny

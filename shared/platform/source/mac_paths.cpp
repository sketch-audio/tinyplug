#include "../platform_paths.hpp"

#include <CoreFoundation/CoreFoundation.h>

#include <pwd.h>
#include <unistd.h>
#include <sys/types.h>

#include "tinyplug/tinyplug.h"

namespace tiny {

auto Platform_paths::format_readable(const std::string& bundle_id) -> std::filesystem::path
{
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
    auto pw = getpwuid(getuid()); // See: https://developer.apple.com/forums/thread/107593
    if (!pw || !pw->pw_dir) return {};
    return std::filesystem::path{pw->pw_dir} / "Library" / "Application Support" / subpath.manufacturer / subpath.product;
}

} // namespace tiny
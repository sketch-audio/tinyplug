#include "../platform_paths.hpp"

#include <array>

#include <windows.h>

namespace tiny {

auto Platform_paths::format_readable(const std::string& /*bundle_id*/) -> std::filesystem::path
{
    auto hmodule = HMODULE{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&format_readable), &hmodule)) {
        return {};
    }

    auto path = std::array<wchar_t, MAX_PATH>{};
    auto size = GetModuleFileNameW(hmodule, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0 || size == path.size()) {
        return {};
    }

    // On Windows, we use a pseudo-bundle structure:
    // - MyPlugin.aaxplugin/Contents/x64       /MyPlugin.aaxplugin
    // - MyPlugin.clap     /Contents/x86_64-win/MyPlugin.clap (CLAP mirrors VST3 structure)
    // - MyPlugin.vst3     /Contents/x86_64-win/MyPlugin.vst3

    // So the "resources" will be located at:
    // - MyPlugin.bundle/Contents/Resources/
    const auto dll_path = std::filesystem::path{path.data(), path.data() + size};
    const auto bundle_path = dll_path.parent_path().parent_path().parent_path();
    const auto resource_path = bundle_path / "Contents" / "Resources";
    
    return resource_path;
}

auto Platform_paths::shared_writable(const Subpath& subpath) -> std::filesystem::path
{
    const auto* env = std::getenv("APPDATA");
    if (!env) return {};
    return std::filesystem::path{env} / subpath.manufacturer / subpath.product;
}

} // namespace tiny
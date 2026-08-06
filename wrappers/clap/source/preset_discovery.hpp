#pragma once

#include <filesystem>
#include <string>
#include <system_error>

#include "clap/helpers/preset-discovery-provider.hh"
#include "clap/helpers/preset-discovery-provider.hxx"

#include <tinyplug/platform_defs.hpp>
#include <tiny_platform/platform_paths.hpp>
#include "plug_info.hpp"

namespace tiny::clap {

using Misbehaviour_handler = ::clap::helpers::MisbehaviourHandler;
using Checking_level = ::clap::helpers::CheckingLevel;

// Shared by both providers: a preset location is only worth declaring if the host can
// actually crawl it. `exists` can throw on an unreadable path, and `init` is `noexcept`.
inline auto location_exists(const std::filesystem::path& path) -> bool
{
    auto error = std::error_code{};
    return std::filesystem::is_directory(path, error) && !error;
}

// CLAP Validator workaround.
// clap_preset_discovery_location_t::location must satisfy two different, apparently
// unrelated consumers on Windows:
//  - clap-validator's location-declare check, which (confirmed empirically, not by spec)
//    is a naive "does the raw string's first character equal '/'" test -- it is not URI
//    aware. Wrapping the path as a real "file://..." URI actually *fails* this check,
//    since that string starts with 'f', not '/' (validator's own suggested fix was
//    nonsensically "prepend '/' before file://").
//  - the crawler's actual file access, which (per the CLAP header's own comment: "works
//    with the OS file system functions (open/stat/...)") wants a string usable directly
//    by Windows path APIs -- i.e. still fundamentally a native path, not a URI.
// So: keep it a native path (backslashes, drive letter untouched) and just prepend a
// single leading '/', which Windows path APIs tolerate. Do not go further into real
// file:// URI territory (scheme, forward slashes, percent-encoding) -- that satisfies
// neither consumer, as tested.
inline auto location_uri_path(const std::filesystem::path& path) -> std::string
{
    auto str = path.string();
#if TINY_PLATFORM_WINDOWS
    if (!str.empty() && str.front() != '/') {
        str.insert(str.begin(), '/');
    }
#endif
    return str;
}

#if defined(NDEBUG)
using Preset_discovery_base = ::clap::helpers::PresetDiscoveryProvider<Misbehaviour_handler::Ignore, Checking_level::None>;
#else
using Preset_discovery_base = ::clap::helpers::PresetDiscoveryProvider<Misbehaviour_handler::Ignore, Checking_level::Maximal>; // REAPER sensitive here.
#endif

// MARK: - Factory Presets

class Factory_presets : public Preset_discovery_base {
public:

    static constexpr auto plugin_id = clap_universal_plugin_id_t{
        .abi = "clap",
        .id = Plug_info::base_identifier,
    };

    static constexpr auto descriptor = clap_preset_discovery_provider_descriptor_t{
        .clap_version = CLAP_VERSION_INIT,
        .id = Plug_info::Clap::factory_preset_discovery_id,
        .name = "Factory Presets Provider",
        .vendor = Plug_info::company_name,
    };

    static constexpr auto filetype = clap_preset_discovery_filetype_t{
        .name = "Factory Preset",
        .description = "Factory preset filetype",
        .file_extension = Plug_info::Presets::extension,
    };

    explicit Factory_presets(const clap_preset_discovery_indexer* indexer)
        : Preset_discovery_base{&descriptor, indexer}
    {}

    auto init() noexcept -> bool override
    {
        indexer()->declare_filetype(indexer(), &filetype);

        const auto bundle_id = std::string{Plug_info::base_identifier} + ".clap";
        const auto location_path = Platform_paths::format_readable(bundle_id);

        // Only declare a location that actually exists. (We may not have bundled presets).
        if (!location_exists(location_path)) {
            return true;
        }

        const auto path_str = location_uri_path(location_path);

        const auto location = clap_preset_discovery_location_t{
            .flags = CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT,
            .name = "Factory Presets",
            .kind = CLAP_PRESET_DISCOVERY_LOCATION_FILE,
            .location = path_str.c_str(),
        };
        indexer()->declare_location(indexer(), &location);

        return true;
    }

    auto getMetadata(uint32_t location_kind, const char* location, const clap_preset_discovery_metadata_receiver_t* metadata_receiver) noexcept -> bool override
    {
        if (location_kind != CLAP_PRESET_DISCOVERY_LOCATION_FILE) return false;
        if (!location) return false;
        if (!metadata_receiver) return false;

        const auto fs_path = std::filesystem::path{location};
        const auto name = fs_path.stem().string();
        if (!metadata_receiver->begin_preset(metadata_receiver, name.c_str(), name.c_str())) return false;
        metadata_receiver->add_plugin_id(metadata_receiver, &plugin_id);
        
        return true;
    }
};

// MARK: - User Presets

class User_presets : public Preset_discovery_base {
public:

    static constexpr auto plugin_id = clap_universal_plugin_id_t{
        .abi = "clap",
        .id = Plug_info::base_identifier,
    };

    static constexpr auto descriptor = clap_preset_discovery_provider_descriptor_t{
        .clap_version = CLAP_VERSION_INIT,
        .id = Plug_info::Clap::user_preset_discovery_id,
        .name = "User Presets Provider",
        .vendor = Plug_info::company_name,
    };

    static constexpr auto filetype = clap_preset_discovery_filetype_t{
        .name = "User Preset",
        .description = "User preset filetype",
        .file_extension = Plug_info::Presets::extension,
    };

    explicit User_presets(const clap_preset_discovery_indexer* indexer)
        : Preset_discovery_base{&descriptor, indexer}
    {}

    auto init() noexcept -> bool override
    {
        indexer()->declare_filetype(indexer(), &filetype);

        const auto location_path = Platform_paths::shared_writable({
            .manufacturer = Plug_info::company_directory_name,
            .product = Plug_info::product_directory_name,
        });

        // Only declare a location that actually exists. (We may not have the user preset directory yet.)
        if (!location_exists(location_path)) {
            return true;
        }

        const auto path_str = location_uri_path(location_path);

        const auto location = clap_preset_discovery_location_t{
            .flags = CLAP_PRESET_DISCOVERY_IS_USER_CONTENT,
            .name = "User Presets",
            .kind = CLAP_PRESET_DISCOVERY_LOCATION_FILE,
            .location = path_str.c_str(),
        };
        indexer()->declare_location(indexer(), &location);

        return true;
    }

    auto getMetadata(uint32_t location_kind, const char* location, const clap_preset_discovery_metadata_receiver_t* metadata_receiver) noexcept -> bool override
    {
        if (location_kind != CLAP_PRESET_DISCOVERY_LOCATION_FILE) return false;
        if (!location) return false;
        if (!metadata_receiver) return false;

        const auto fs_path = std::filesystem::path{location};
        const auto name = fs_path.stem().string();
        if (!metadata_receiver->begin_preset(metadata_receiver, name.c_str(), name.c_str())) return false;
        metadata_receiver->add_plugin_id(metadata_receiver, &plugin_id);
        
        return true;
    }
};

} // namespace tiny::clap
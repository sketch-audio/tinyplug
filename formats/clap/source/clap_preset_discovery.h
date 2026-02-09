#pragma once

#include <filesystem>
#include <string>

#include "clap/helpers/preset-discovery-provider.hh"
#include "clap/helpers/preset-discovery-provider.hxx"

#include "platform/platform_paths.hpp"
#include "plug_info.h"

namespace tiny {

using Misbehaviour_handler = clap::helpers::MisbehaviourHandler;
using Checking_level = clap::helpers::CheckingLevel;
using Preset_discovery_base = clap::helpers::PresetDiscoveryProvider<Misbehaviour_handler::Ignore, Checking_level::Minimal>;

// MARK: - Factory Presets

class Clap_factory_presets : public Preset_discovery_base {
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

    explicit Clap_factory_presets(const clap_preset_discovery_indexer* indexer)
        : Preset_discovery_base{&descriptor, indexer}
    {}

    auto init() noexcept -> bool override
    {
        indexer()->declare_filetype(indexer(), &filetype);

        const auto bundle_id = std::string{Plug_info::base_identifier} + ".clap";
        const auto location_path = Platform_paths::format_readable(bundle_id);

        const auto location = clap_preset_discovery_location{
            .flags = CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT,
            .name = "Factory Presets",
            .kind = CLAP_PRESET_DISCOVERY_LOCATION_FILE,
            .location = location_path.c_str(),
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

// User Presets

class Clap_user_presets : public Preset_discovery_base {
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

    explicit Clap_user_presets(const clap_preset_discovery_indexer* indexer)
        : Preset_discovery_base{&descriptor, indexer}
    {}

    auto init() noexcept -> bool override
    {
        indexer()->declare_filetype(indexer(), &filetype);

        const auto location_path = Platform_paths::shared_writable({
            .manufacturer = Plug_info::company_directory_name,
            .product = Plug_info::product_directory_name,
        });

        const auto location = clap_preset_discovery_location{
            .flags = CLAP_PRESET_DISCOVERY_IS_USER_CONTENT,
            .name = "User Presets",
            .kind = CLAP_PRESET_DISCOVERY_LOCATION_FILE,
            .location = location_path.c_str(),
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

} // namespace tiny
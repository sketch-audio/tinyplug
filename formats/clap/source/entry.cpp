#include "clap/clap.h"

#include "plugin.hpp"
#include "preset_discovery.hpp"

namespace tiny {

static const auto pluginFactory = clap_plugin_factory_t{
    .get_plugin_count = [](const clap_plugin_factory*) -> uint32_t {
        return 1;
    },
    .get_plugin_descriptor = [](const clap_plugin_factory*, uint32_t index) -> const clap_plugin_descriptor_t* {
        return index == 0 ? &clap::Plugin::descriptor : nullptr;
    },
    .create_plugin = [](const clap_plugin_factory*, const clap_host_t* host, const char* pluginID) -> const clap_plugin_t* {
        if (!clap_version_is_compatible(host->clap_version) || strcmp(pluginID, clap::Plugin::descriptor.id)) {
            return nullptr;
        }
        //
        auto plugin = new clap::Plugin(host);
        return plugin->clapPlugin();
    },
};

static const auto presetDiscoveryFactory = clap_preset_discovery_factory_t{
    .count = [](const clap_preset_discovery_factory*) -> uint32_t {
        return 2;
    },
    .get_descriptor = [](const clap_preset_discovery_factory*, uint32_t index) -> const clap_preset_discovery_provider_descriptor_t* {
        if (index == 0) {
            return &clap::Factory_presets::descriptor;
        }

        if (index == 1) {
            return &clap::User_presets::descriptor;
        }

        return nullptr;
    },
    .create = [](const clap_preset_discovery_factory*, const clap_preset_discovery_indexer_t* indexer, const char* provider_id) -> const clap_preset_discovery_provider_t* {
        if (strcmp(provider_id, clap::Factory_presets::descriptor.id) == 0) {
            auto provider = new clap::Factory_presets{indexer};
            return provider->provider();
        }

        if (strcmp(provider_id, clap::User_presets::descriptor.id) == 0) {
            auto provider = new clap::User_presets{indexer};
            return provider->provider();
        }

        return nullptr;
    },
};

} // namespace tiny

// MARK: - Entry
extern "C" {
    CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
        .clap_version = CLAP_VERSION,
        .init = [](const char*) -> bool {
            return true;
        },
        .deinit = []() {
        },
        .get_factory = [](const char* factoryID) -> const void* {
            if (strcmp(factoryID, CLAP_PLUGIN_FACTORY_ID) == 0) {
                return &tiny::pluginFactory;
            }

            if (strcmp(factoryID, CLAP_PRESET_DISCOVERY_FACTORY_ID) == 0) {
                return &tiny::presetDiscoveryFactory;
            }

            return nullptr;
        },
    };
}
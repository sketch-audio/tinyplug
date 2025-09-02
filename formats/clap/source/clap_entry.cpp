#include "clap/clap.h"

#include "clap_plugin.h"

namespace tiny {

static const clap_plugin_factory_t pluginFactory = {
    .get_plugin_count = [] (const clap_plugin_factory*) -> uint32_t {
        return 1;
    },

    .get_plugin_descriptor = [] (const clap_plugin_factory*, uint32_t index) -> const clap_plugin_descriptor_t* {
        return index == 0 ? &Clap_plugin::descriptor : nullptr;
    },

    .create_plugin = [] (const clap_plugin_factory*, const clap_host_t* host, const char* pluginID) -> const clap_plugin_t* {
        if (!clap_version_is_compatible(host->clap_version) || strcmp(pluginID, Clap_plugin::descriptor.id)) {
            return nullptr;
        }
        //
        auto plugin = new Clap_plugin(host);
        return plugin->clapPlugin();
    },
};

}

// MARK: - Entry
extern "C" {
    CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
        .clap_version = CLAP_VERSION,

        .init = [] (const char*) -> bool {
            return true;
        },

        .deinit = [] () {
            //
        },

        .get_factory = [] (const char* factoryID) -> const void* {
            return strcmp(factoryID, CLAP_PLUGIN_FACTORY_ID) ? nullptr : &tiny::pluginFactory;
        },
    };
}
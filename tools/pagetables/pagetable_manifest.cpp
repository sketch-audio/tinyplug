#include <iostream>

#include <tiny_models.hpp> // Core + this plug-in's models; tools need nothing model-aware.
#include <nlohmann/json.hpp>

#include "plug_info.hpp"

auto main() -> int
{
    using namespace tiny;
    const auto& specs = User_params::param_specs(params::Param_order::Presentation);

    auto params = nlohmann::json::array();
    for (auto i = uint32_t{}; i < User_params::num_params; ++i) {
        const auto& s = specs[i];
        const char* policy = [&]() -> const char* {
            switch (s.policy) {
                case params::Policy::Automation: return "automation";
                case params::Policy::Control:    return "control";
                case params::Policy::Hidden:     return "hidden";
                case params::Policy::Interface:  return "interface";
            }
            return "unknown";
        }();
        params.push_back({
            {"index",      i},
            {"address",    s.identity.address},
            {"name",       std::string{s.name}},
            {"short_name", std::string{s.short_name}},
            {"policy",     policy}
        });
    }

    // FOURCC uint32_t → 4-char string.
    auto fourcc = [](uint32_t v) -> std::string {
        return {
            char((v >> 24) & 0xFF),
            char((v >> 16) & 0xFF),
            char((v >>  8) & 0xFF),
            char( v        & 0xFF)
        };
    };

    std::cout << nlohmann::json{
        {"manufacturer_code", fourcc(Plug_info::Aax::manufacturer_id)},
        {"product_code",      fourcc(Plug_info::Aax::product_id)},
        {"plugin_id",         static_cast<int>(Plug_info::Aax::plugin_id)},
        {"can_process_mono",  Plug_info::can_process_mono},
        {"base_file_name",    std::string{Plug_info::base_file_name}},
        {"plugin_name",       std::string{Plug_info::plugin_name}},
        {"company_name",      std::string{Plug_info::company_name}},
        {"params",            params}
    }.dump(2) << "\n";

    return 0;
}

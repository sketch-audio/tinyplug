#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <iomanip>
#include <sstream>

#include <tinyplug/tinyplug.h>
#include <nlohmann/json.hpp>

// AAX SDK Headers
#include "AAX.h"
#include "AAX_CChunkDataParser.h"
#include "AAX_EndianSwap.h"
#include "../formats/aax/source/aax_adapters.h"

// User model.
#include "models/param_model.h"
#include "plug_info.h"

#ifndef PRESET_DIR
#define PRESET_DIR ""
#endif

auto main() -> int
{
    using namespace tiny;

    const auto preset_path = std::filesystem::path{PRESET_DIR};
    const auto path_exists = std::filesystem::exists(preset_path);
    const auto path_is_directory = std::filesystem::is_directory(preset_path);
    if (!path_exists || !path_is_directory) {
        std::cerr << "Preset directory does not exist: " << preset_path << "\n";
        return 1;
    }

    using User_params = Param_infos<Param_model>;
    const auto defaults = User_params::make_defaults<double>(Value_space::Knob);

    // State adapter to convert between JSON and parameter values.
    auto state_adapter = State_adapter{{
        .load_model = [&]() {
            return State_adapter::Load_model{
                .param_tree = &User_params::param_tree(),
                .num_params = User_params::num_params
            };
        }
    }};

    const auto custom_extension = std::string{"."} + std::string{Plug_info::Presets::extension}; // Have to add the dot.

    for (const auto& entry : std::filesystem::directory_iterator{preset_path}) {
        if (!entry.is_regular_file()) continue;

        if (entry.path().extension() != custom_extension) {
            std::cout << "Skipping non-preset file: " << entry.path() << "\n";
            continue;
        };

        std::cout << "Processing preset file: " << entry.path() << "\n";

        // Load the preset file.
        auto file = std::ifstream{entry.path()};
        if (!file) {
            std::cerr << "Failed to open preset file: " << entry.path() << "\n";
            continue;
        }

        // Obtain the JSON.
        auto preset_json = nlohmann::ordered_json{};
        try {
            file >> preset_json;
        }
        catch (...) {
            std::cerr << "Failed to parse JSON in preset file: " << entry.path() << "\n";
            continue;
        }

        // Extract values.
        const auto preset_values = state_adapter.param_values(preset_json);
        const auto editor_state = state_adapter.editor_state(preset_json);

        // These are knob values or State_rules::no_value.
        auto state_values = std::vector<float>(preset_values.size(), State_rules::no_value);
        for (size_t i = 0; i < preset_values.size(); ++i) {
            if (preset_values[i].has_value()) {
                state_values[i] = static_cast<float>(*(preset_values[i]));
            }
        }

        // Make out dir (PRESETS_DIR/out).
        auto out_path = preset_path / "out";
        std::filesystem::create_directory(out_path);
        out_path /= entry.path().stem();

        // MARK: - AAX
        out_path.replace_extension(".tfx");

        const auto num_params = state_values.size();
        auto chunk_parser = AAX_CChunkDataParser{};
        // Add the number of parameters and the edit keys.

        const auto edit_keys = join_keys(editor_state);
        chunk_parser.AddInt32(State_rules::Aax::num_params, static_cast<int32_t>(num_params));
        chunk_parser.AddString(State_rules::Aax::edit_keys, edit_keys.c_str());

        // Add the parameter values.
        for (auto i = decltype(num_params){}; i < num_params; ++i) {
            if (auto aax_id = tiny::tiny_id_to_aax(i); aax_id.has_value()) {
                const auto* id_cstr = (*aax_id).c_str();

                const auto& spec = User_params::param_spec(i);

                // For AAX we need plain value or State_rules::no_value.
                const auto to_write = [&]() {
                    if (state_values[i] == State_rules::no_value) {
                        return State_rules::no_value;
                    }
                    else {
                        const auto as_double = static_cast<double>(state_values[i]);
                        const auto plain = Value_conv::knob_to_plain(as_double, spec.semantics);
                        return static_cast<float>(plain);
                    }
                }();
                chunk_parser.AddFloat(id_cstr, to_write);
            }
        }

        for (const auto& [key, val] : editor_state) {
            const auto tag = tag_for(val);

            switch (tag) {
                case State_tag::Bool: {
                    if (const auto b = std::get_if<bool>(&val)) {
                        chunk_parser.AddInt32(key.c_str(), *b ? 1 : 0);
                    }
                    break;
                }
                case State_tag::Int: {
                    if (const auto i = std::get_if<int32_t>(&val)) {
                        chunk_parser.AddInt32(key.c_str(), *i);
                    }
                    break;
                }
                case State_tag::Double: {
                    if (const auto d = std::get_if<double>(&val)) {
                        chunk_parser.AddDouble(key.c_str(), *d);
                        break;
                    }
                }
                case State_tag::String: {
                    if (const auto s = std::get_if<std::string>(&val)) {
                        chunk_parser.AddString(key.c_str(), (*s).c_str());
                    }
                    break;
                }
                default: {
                    break;
                }
            }
        }

        auto chunk_size = chunk_parser.GetChunkDataSize();

        // 1. Calculate total size: Header size + Data size - 1 (for the char fData[1])
        const size_t total_chunk_mem_size = sizeof(AAX_SPlugInChunk) + chunk_size - 1;

        // 2. Allocate a buffer to hold the structure + the trailing data
        std::vector<uint8_t> chunk_buffer(total_chunk_mem_size, 0);

        // 3. Cast the start of the buffer to a pointer of the struct type
        auto* schunk_ptr = reinterpret_cast<AAX_SPlugInChunk*>(chunk_buffer.data());

        // 4. Populate the header fields
        //schunk_ptr->fSize = AAX_BigEndianNativeSwap(static_cast<int32_t>(chunk_size));
        schunk_ptr->fVersion = AAX_BigEndianNativeSwap(int32_t{-1}); // Or chunk_parser.GetChunkVersion()
        schunk_ptr->fManufacturerID = AAX_BigEndianNativeSwap(Plug_info::Aax::manufacturer_id);
        schunk_ptr->fProductID = AAX_BigEndianNativeSwap(Plug_info::Aax::product_id);
        schunk_ptr->fPlugInID = AAX_BigEndianNativeSwap(Plug_info::Aax::plugin_id);
        schunk_ptr->fChunkID = AAX_BigEndianNativeSwap(AAX_CTypeID(State_rules::Aax::chunk_id));

        // Use std::memset and std::strncpy for the fixed-size array fName
        std::memset(schunk_ptr->fName, 0, 32);
        std::strncpy(reinterpret_cast<char*>(schunk_ptr->fName), "AAX Plug-in State", 31);

        // 5. Tell the parser to write directly into the fData member
        // Since we allocated enough space, it will safely overflow fData[1] 
        // and fill the rest of chunk_buffer.
        chunk_parser.GetChunkData(schunk_ptr);
        schunk_ptr->fSize = AAX_BigEndianNativeSwap(static_cast<int32_t>(total_chunk_mem_size));


        std::ofstream tfx_file(out_path, std::ios::binary);
        if (!tfx_file) {
            std::cerr << "Failed to open output AAX preset file: " << out_path << "\n";
            continue;
        }
        tfx_file.write(reinterpret_cast<const char*>(chunk_buffer.data()), chunk_buffer.size());
    }

    return 0;
}
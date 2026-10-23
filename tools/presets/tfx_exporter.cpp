#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <iomanip>
#include <sstream>

#include <tinyplug/tinyplug.hpp>
#include <nlohmann/json.hpp>

// AAX SDK Headers
#include "AAX.h"
#include "AAX_CChunkDataParser.h"
#include "AAX_EndianSwap.h"
#include "chunk_writer.hpp" // The AAX wrapper's chunk writer (on the include path via cmake)

#include "plug_info.hpp"

#ifndef PRESET_DIR
#define PRESET_DIR ""
#endif

auto main(int argc, char** argv) -> int
{
    using namespace tiny;

    const auto preset_path = std::filesystem::path{argc > 1 ? argv[1] : PRESET_DIR}; // An argument overrides the built-in directory.
    const auto path_exists = std::filesystem::exists(preset_path);
    const auto path_is_directory = std::filesystem::is_directory(preset_path);
    if (!path_exists || !path_is_directory) {
        std::cerr << "Preset directory does not exist: " << preset_path << "\n";
        return 1;
    }

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

        // Plain values, `no_value` for a parameter a session doesn't save, as the wrapper writes them.
        const auto preset_values = state_adapter.param_values(preset_json);
        auto values = std::vector<float>(preset_values.size(), State_rules::no_value);
        for (auto i = size_t{}; i < preset_values.size() && i < User_params::num_params; ++i) {
            const auto& spec = User_params::param_spec(static_cast<uint32_t>(i));
            if (preset_values[i] && State_rules::is_persistent(spec)) {
                values[i] = static_cast<float>(params::knob_to_plain(*preset_values[i], spec.semantics));
            }
        }
        const auto state_record = state_adapter.state_record(preset_json); // Opaque: passed through as the wrapper stores it.

        // The editor keys: an older preset's author keys, and the framework's keys naming the preset, which
        // make a host's load of the file a preset load. A preset carries no editor record and no bypass.
        auto editor = state_adapter.editor_state(preset_json);
        const auto info = state_adapter.preset_info(preset_json);
        framework_keys::write(editor, Framework_keys{
            .preset_name = info.name.empty() ? entry.path().stem().string() : info.name,
            .preset_id = info.id,
        });

        // Make out dir (PRESETS_DIR/out).
        auto out_path = preset_path / "out";
        std::filesystem::create_directory(out_path);
        out_path /= entry.path().stem();
        out_path.replace_extension(".tfx");

        auto chunk_parser = AAX_CChunkDataParser{};
        aax::write_chunk(chunk_parser, aax::Chunk_contents{.plain_values = values, .editor = &editor, .record = state_record});

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
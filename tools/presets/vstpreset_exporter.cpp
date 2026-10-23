#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <iomanip>
#include <sstream>

#include <tiny_models.hpp> // Core + this plug-in's models; tools need nothing model-aware.
#include <nlohmann/json.hpp>

// VST3 SDK Headers
#include "public.sdk/source/vst/vstpresetfile.h"
#include "public.sdk/source/vst/utility/memoryibstream.h"
#include "base/source/fstreamer.h"

#include "plug_info.hpp"
#include "chunks.hpp" // The VST3 wrapper's chunk writers (on the include path via cmake)

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
        //
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

        // Values in knob space, `no_value` for a parameter a session doesn't save, as the wrapper writes them.
        const auto preset_values = state_adapter.param_values(preset_json);
        auto values = std::vector<float>(preset_values.size(), State_rules::no_value);
        for (auto i = size_t{}; i < preset_values.size() && i < User_params::num_params; ++i) {
            const auto& spec = User_params::param_spec(static_cast<uint32_t>(i));
            if (preset_values[i] && State_rules::is_persistent(spec)) values[i] = static_cast<float>(*preset_values[i]);
        }
        const auto state_record = state_adapter.state_record(preset_json); // Opaque: passed through as the wrapper stores it.

        // The editor map: an older preset's author keys, and the framework's keys naming the preset, which
        // make a host's load of the file a preset load. A preset carries no editor record.
        auto pairs = state_adapter.editor_state(preset_json);
        const auto info = state_adapter.preset_info(preset_json);
        framework_keys::write(pairs, Framework_keys{
            .preset_name = info.name.empty() ? entry.path().stem().string() : info.name,
            .preset_id = info.id,
        });

        // Make out dir (PRESETS_DIR/out).
        auto out_path = preset_path / "out";
        std::filesystem::create_directory(out_path);
        out_path /= entry.path().stem();
        out_path.replace_extension(".vstpreset");

        auto processor_stream = Steinberg::ResizableMemoryIBStream{};
        auto controller_stream = Steinberg::ResizableMemoryIBStream{};
        if (!vst3::write_processor_chunk(&processor_stream, values, State_rules::no_value, state_record)
            || !vst3::write_controller_chunk(&controller_stream, pairs, {})) {
            std::cerr << "Failed to build the chunks for: " << entry.path() << "\n";
            continue;
        }

        using namespace Steinberg;
        using namespace Steinberg::Vst;
        if (auto stream = FileStream::open(out_path.string().c_str(), "wb")) {
            auto preset_file = PresetFile{stream};
            preset_file.setClassID(FUID(
                Plug_info::Vst3::processor_uid[0],
                Plug_info::Vst3::processor_uid[1],
                Plug_info::Vst3::processor_uid[2],
                Plug_info::Vst3::processor_uid[3]
            ));
            preset_file.writeHeader();
            // Store component state.
            preset_file.writeChunk(processor_stream.getData(), static_cast<int32>(processor_stream.getCursor()), ChunkType::kComponentState);
            preset_file.writeChunk(controller_stream.getData(), static_cast<int32>(controller_stream.getCursor()), ChunkType::kControllerState);
            preset_file.writeChunkList();
            stream->release();
        }
        else {
            std::cerr << "Failed to open output VST preset file: " << out_path << "\n";
            continue;
        }
    }

    return 0;
}
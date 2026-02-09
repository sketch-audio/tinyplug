#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <iomanip>
#include <sstream>

#include <tinyplug/tinyplug.h>
#include <nlohmann/json.hpp>

// VST3 SDK Headers
#include "public.sdk/source/vst/vstpresetfile.h"
#include "public.sdk/source/vst/utility/memoryibstream.h"
#include "base/source/fstreamer.h"

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

        // MARK: - VST
        out_path.replace_extension(".vstpreset");

        auto state_header = std::array<uint32_t, 4>{
            Plug_info::framework_code,
            Plug_info::manufacturer_code,
            Plug_info::plugin_code,
            static_cast<uint32_t>(state_values.size())
        };

        auto processor_data = std::vector<std::byte>{};
        processor_data.insert(processor_data.end(),
            reinterpret_cast<std::byte*>(state_header.data()),
            reinterpret_cast<std::byte*>(state_header.data() + state_header.size())
        );
        processor_data.insert(processor_data.end(),
            reinterpret_cast<std::byte*>(state_values.data()),
            reinterpret_cast<std::byte*>(state_values.data() + state_values.size())
        );

        // Header is shared.
        state_header[3] = static_cast<uint32_t>(editor_state.size());

        auto memory_stream = Steinberg::ResizableMemoryIBStream{};
        auto streamer = Steinberg::IBStreamer{&memory_stream};

        streamer.writeInt32uArray(state_header.data(), static_cast<int32_t>(state_header.size()));

        // Helper
        auto write_container = [&](const auto& container) {
            const auto num = static_cast<uint32_t>(container.size());
            if (!streamer.writeInt32u(num)) {
                return false;
            }
            if (num > 0) {
                if (!streamer.writeRaw(container.data(), sizeof(container[0]) * num)) {
                    return false;
                }
            }
            return true;
        };

        for (const auto& [key, val] : editor_state) {
            write_container(key);
            
            const auto tag = tag_for(val);
            streamer.writeInt32u(enum_raw(tag));

            switch (tag) {
                case State_tag::Bool: {
                    streamer.writeBool(*std::get_if<bool>(&val));
                    break;
                }
                case State_tag::Int: {
                    streamer.writeInt32(*std::get_if<int32_t>(&val));
                    break;
                }
                case State_tag::Double: {
                    streamer.writeDouble(*std::get_if<double>(&val));
                    break;
                }
                case State_tag::String: {
                    write_container(*std::get_if<std::string>(&val));
                    break;
                }
                default: break;
            }
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
            preset_file.writeChunk(processor_data.data(), static_cast<int32>(processor_data.size()), ChunkType::kComponentState);
            preset_file.writeChunk(memory_stream.getData(), static_cast<int32>(memory_stream.getCursor()), ChunkType::kControllerState);
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
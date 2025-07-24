#include <algorithm>
#include <optional>
#include <ranges>

#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "public.sdk/source/vst/utility/stringconvert.h"
#include "public.sdk/source/vst/vstaudioeffect.h"
#include "base/source/fstreamer.h"

#include "plug_info.h"
#include "user/param_model.h"

#include "vst3_adapters.h"
#include "vst3_processor.h"

Vst3_processor::Vst3_processor()
{
    setControllerClass(tiny::vst3::map_to_fuid(tiny::Plug_info::Vst3::controller_uid));
}

Vst3_processor::~Vst3_processor()
{
    //
}

Steinberg::tresult PLUGIN_API Vst3_processor::initialize(Steinberg::FUnknown* context)
{
    // Here the Plug-in will be instantiated.

    // Initialize the parent.
    Steinberg::tresult result = Steinberg::Vst::AudioEffect::initialize(context);

    if (result != Steinberg::kResultOk)
        return result;
    
    // Create the audio IO.

    using namespace Steinberg::Vst;
    using namespace tiny;

    const auto input_count = Plug_info::wants_sidechain ? 2 : 1;

    for (size_t i = 0; i < input_count; ++i) {
        const auto is_main = (i == 0);

        const auto* input_name = is_main ? u"Input" : u"Sidechain";
        const auto bus_type = is_main ? BusTypes::kMain : BusTypes::kAux;

        addAudioInput(input_name, SpeakerArr::kStereo, bus_type);
    }

    addAudioOutput(u"Output", SpeakerArr::kStereo, BusTypes::kMain);

    // Create the event IO.

    _events.reserve(128); // Want fixed size event vector.

    // Get knob defaults for automation points.
    for (const auto& param : _params.get_kernel_specs()) {
        const auto idx = to_underlying(param.id);
        _lpoints[idx] = {.offset = -1, .value = get_knob_default(param)};
    }

    return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API Vst3_processor::terminate()
{
    // Here the Plug-in will be de-instantiated, last possibility to remove some memory!

    // Do not forget to call parent.
    return Steinberg::Vst::AudioEffect::terminate();
}

Steinberg::tresult PLUGIN_API Vst3_processor::setActive(Steinberg::TBool state)
{
    // Called when the Plug-in is enable/disable (On/Off).
	return Steinberg::Vst::AudioEffect::setActive(state);
}

Steinberg::tresult PLUGIN_API Vst3_processor::setupProcessing(Steinberg::Vst::ProcessSetup& newSetup)
{
    // Called before any processing.
    _kernel->reset(newSetup.sampleRate, newSetup.maxSamplesPerBlock);
    return Steinberg::Vst::AudioEffect::setupProcessing(newSetup);
}

Steinberg::tresult PLUGIN_API Vst3_processor::canProcessSampleSize(Steinberg::int32 symbolicSampleSize)
{
	// By default kSample32 is supported.
	if (symbolicSampleSize == Steinberg::Vst::kSample32)
		return Steinberg::kResultTrue;

	return Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API Vst3_processor::process(Steinberg::Vst::ProcessData& data)
{
    this->normalize_input_events(data);

    // Now we have the events organized how we want.
    const auto event_count = _events.size();
    auto event_index = size_t{};
    const auto* event = event_count > 0 ? &_events[event_index] : nullptr;

    auto next_event = [&]() {
        ++event_index;
        if (event_index >= event_count) {
            event = nullptr;
        }
        else {
            event = &_events[event_index];
        }
    };

    // Do the while loop.
    auto do_process = [this, &data](size_t num_frames, size_t offset) {
        // Assign buffer ptrs.
        _ibuffers[0] = &data.inputs->channelBuffers32[0][offset];
        _ibuffers[1] = &data.inputs->channelBuffers32[1][offset];

        if constexpr (tiny::Plug_info::wants_sidechain) {
            _ibuffers[2] = &data.inputs->channelBuffers32[0][offset];
            _ibuffers[3] = &data.inputs->channelBuffers32[1][offset];
        }

        _obuffers[0] = &data.outputs->channelBuffers32[0][offset];
        _obuffers[1] = &data.outputs->channelBuffers32[1][offset];

        // Process kernel.
        auto context = tiny::Dsp_context{
            .ibuffers = _ibuffers,
            .obuffers = _obuffers,
            .num_frames = num_frames
        };
        _kernel->process(context);
    };

    const auto frame_count = data.numSamples;
    auto now = decltype(frame_count){};
    auto remaining = frame_count;

    while (remaining > 0) {
        if (!event) {
            const auto offset = frame_count - remaining;
            do_process(remaining, offset);
            break;
        }

        const auto frames_until_event = std::max({}, event->offset - now);

        if (frames_until_event > 0) {
            const auto offset = frame_count - remaining;
            do_process(frames_until_event, offset);
            remaining -= frames_until_event;
            now += frames_until_event;
        }

        do {
            _kernel->handle_event(event->event);
            next_event();
        } while (event && event->offset <= now);
    }

    return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API Vst3_processor::setState(Steinberg::IBStream* state)
{
    if (!state)
        return Steinberg::kResultFalse;

    return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API Vst3_processor::getState(Steinberg::IBStream* state)
{
    if (!state)
        return Steinberg::kResultFalse;

    return Steinberg::kResultOk;
}

// MARK: - private

auto Vst3_processor::normalize_input_events(Steinberg::Vst::ProcessData& data) -> void
{
    using namespace tiny;

    _events.clear(); // 

    auto& param_changes = *data.inputParameterChanges;
    const auto num_changes = param_changes.getParameterCount();

    const auto& specs = _params.get_kernel_specs();

    for (auto i = decltype(num_changes){}; i < num_changes; ++i) {
        auto& queue = *param_changes.getParameterData(i);

        const auto id = queue.getParameterId();
        const auto& param = specs[id]; // To denormalize the automation values.

        const auto point_count = queue.getPointCount();

        auto& previous = _lpoints[id];

        for (auto point_idx = decltype(point_count){}; point_idx < point_count; ++point_idx) {
            auto value = Steinberg::Vst::ParamValue{};
            auto offset = int32_t{};
            queue.getPoint(point_idx, offset, value);

            const auto ramp_dur = std::max(offset - previous.offset, 0);

            if (_events.size() == _events.capacity()) {
                // _events vector is full!
                std::cout << "Events vector full!\n";
            };

            // Set param
            if (ramp_dur <= 1) {
                _events.push_back({
                    .offset = std::max(previous.offset, {}),
                    .event = Set_param{
                        .id = id,
                        .value = knob_to_plain_space(value, param)
                    }
                });
            }
            // Ramp param
            else {
                _events.push_back({
                    .offset = std::max(previous.offset, {}),
                    .event = Ramp_param{
                        .id = id,
                        .target = knob_to_plain_space(value, param),
                        .dur_samples = ramp_dur
                    }
                });
            }

            previous.offset = offset;
            previous.value = value;
        }

        previous.offset = -1; // Next buffer starts with -1;
    }

    // sort events.
    std::ranges::sort(_events, [](const auto& a, const auto& b) { return a.offset < b.offset; });
}

#pragma once

#include <limits>
#include <memory>
#include <vector>

#include "public.sdk/source/vst/vstaudioeffect.h"

#include "plug_processor.h"
#include "models/meter_model.h"
#include "models/param_model.h"

namespace tiny {

class Vst3_processor : public Steinberg::Vst::AudioEffect {
public:

    using Super = Steinberg::Vst::AudioEffect;
    Vst3_processor() : Super{} {
        setControllerClass(tiny::map_to_fuid(tiny::Plug_info::Vst3::controller_uid));
    }
    ~Vst3_processor() SMTG_OVERRIDE = default;

    // Create function
    static Steinberg::FUnknown* createInstance(void* /*context*/)
    {
        return (Steinberg::Vst::IAudioProcessor*)new Vst3_processor;
    }

    //--- ---------------------------------------------------------------------
    // AudioEffect overrides:
    //--- ---------------------------------------------------------------------
    /** Called at first after constructor */
    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) SMTG_OVERRIDE;

    /** Called at the end before destructor */
    Steinberg::tresult PLUGIN_API terminate() SMTG_OVERRIDE;

    /** Switch the Plug-in on/off */
    Steinberg::tresult PLUGIN_API setActive(Steinberg::TBool state) SMTG_OVERRIDE;

    /** Will be called before any process call */
    Steinberg::tresult PLUGIN_API setupProcessing(Steinberg::Vst::ProcessSetup& newSetup) SMTG_OVERRIDE;

    Steinberg::tresult PLUGIN_API setBusArrangements(Steinberg::Vst::SpeakerArrangement* inputs, Steinberg::int32 numIns, Steinberg::Vst::SpeakerArrangement* outputs, Steinberg::int32 numOuts) SMTG_OVERRIDE;

    /** Asks if a given sample size is supported see SymbolicSampleSizes. */
    Steinberg::tresult PLUGIN_API canProcessSampleSize(Steinberg::int32 symbolicSampleSize) SMTG_OVERRIDE;

    /** Here we go...the process call */
    Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData& data) SMTG_OVERRIDE;

    /** For persistence */
    Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* state) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* state) SMTG_OVERRIDE;

    //
    Steinberg::uint32 PLUGIN_API getLatencySamples() SMTG_OVERRIDE;
    Steinberg::uint32 PLUGIN_API getTailSamples() SMTG_OVERRIDE;
    Steinberg::uint32 PLUGIN_API getProcessContextRequirements() SMTG_OVERRIDE;

private:
    //
    struct Automation_point {
        int32_t offset{-1};
        double value{};
    };

    using User_params = Param_infos<Param_model>;
    using User_meters = Meter_infos<Meter_model>;

    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_meters = User_meters::num_meters;

    static constexpr auto max_ichannels = size_t{2};
    static constexpr auto max_schannels = size_t{2};
    static constexpr auto max_ochannels = size_t{2};

    // Runtime.
    size_t _ichannels{max_ichannels};
    size_t _schannels{Plug_info::wants_sidechain ? max_schannels : 0};
    size_t _ochannels{max_ochannels};

    // Pointers to host io buffers.
    std::array<const float*, max_ichannels> _ibuffers{};
    std::array<const float*, max_schannels> _sbuffers{};
    std::array<float*, max_ochannels> _obuffers{};
    std::array<float, num_meters> _meters{};

    using State_queue = Lock_free_queue<Set_param, 256>;
    State_queue _queue{};

    std::array<Automation_point, num_params> _last_points{};
    std::array<double, num_meters> _last_meters{};

    std::vector<Tagged_event> _events{}; // Some fixed size thing.

    std::unique_ptr<Plug_processor> _processor = std::make_unique<Plug_processor>();
    uint32_t _latency{_processor->latency_samps()};

    using Latency_flag = std::atomic<std::optional<uint32_t>>;
    static_assert(Latency_flag::is_always_lock_free);

    // Communicates the pending latency from `process` to `setActive`.
    Latency_flag _pending_latency{};
    bool _did_reset{}; // Flag to send latency change.

    // Communicates the accepted latency from `setActive` to `process`.
    Latency_flag _accepted_latency{};

    auto normalize_input_events(Steinberg::Vst::ProcessData& data) -> void;

};

} // namespace tiny
#pragma once

#include <limits>
#include <memory>
#include <vector>

#include "public.sdk/source/vst/vstaudioeffect.h"

#include "user/dsp_kernel.h"
#include "user/param_model.h"

class Vst3_processor : public Steinberg::Vst::AudioEffect {
public:
    
    Vst3_processor();
	~Vst3_processor() SMTG_OVERRIDE;

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
	
	/** Asks if a given sample size is supported see SymbolicSampleSizes. */
	Steinberg::tresult PLUGIN_API canProcessSampleSize(Steinberg::int32 symbolicSampleSize) SMTG_OVERRIDE;

	/** Here we go...the process call */
	Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData& data) SMTG_OVERRIDE;
		
	/** For persistence */
	Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* state) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* state) SMTG_OVERRIDE;

private:
    //
    struct Automation_point {
        int32_t offset{-1};
        double value{};
    };

    using User_params = tiny::Params<tiny::Param_model>;
    using User_exports = tiny::Exports<tiny::Param_model>;

    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_exports = User_exports::num_exports;

    static constexpr auto num_ichannels = size_t{2};
    static constexpr auto num_schannels = size_t{2};
    static constexpr auto num_ochannels = size_t{2};

    // Pointers to host io buffers.
    std::array<const float*, num_ichannels> _ibuffers{};
    std::array<const float*, num_schannels> _sbuffers{};
    std::array<float*, num_ochannels> _obuffers{};
    std::array<float, num_exports> _exports{};

    User_params _params{};

    std::array<Automation_point, num_params> _lpoints{};
    std::array<double, num_exports> _lexports{};

    std::vector<tiny::Tagged_event> _events{}; // Some fixed size thing.

    std::unique_ptr<tiny::Dsp_kernel> _kernel = std::make_unique<tiny::Dsp_kernel>();

    auto normalize_input_events(Steinberg::Vst::ProcessData& data) -> void;

};
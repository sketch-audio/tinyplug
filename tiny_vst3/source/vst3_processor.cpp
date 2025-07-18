#include "base/source/fstreamer.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

#include "plug_info.h"
#include "user_plug.h"

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
	return Steinberg::Vst::AudioEffect::setupProcessing(newSetup);
}

Steinberg::tresult PLUGIN_API Vst3_processor::canProcessSampleSize(Steinberg::int32 symbolicSampleSize)
{
	// By default kSample32 is supported.
	if (symbolicSampleSize == Steinberg::Vst::kSample32)
		return Steinberg::kResultTrue;

	return Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API Vst3_processor::process(Steinberg::Vst::ProcessData& /*data*/)
{
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

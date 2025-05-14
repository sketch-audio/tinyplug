#include "base/source/fstreamer.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

#include "shared.h"

#include "adapters.h"
#include "processor.h"

namespace tiny {

Processor::Processor()
{
    setControllerClass(tiny::map_to_fuid(tiny::User_plug::info.vst3_controller_uid));
}

Processor::~Processor()
{
    //
}

Steinberg::tresult PLUGIN_API Processor::initialize(Steinberg::FUnknown* context)
{
    // Here the Plug-in will be instantiated.

    // Initialize the parent.
    Steinberg::tresult result = Steinberg::Vst::AudioEffect::initialize(context);

    if (result != Steinberg::kResultOk)
        return result;
    
    // Create the audio IO.
    const auto& plug_io = tiny::User_plug::io;
    using namespace Steinberg::Vst;

    const auto input_count = plug_io.audio_ports.num_inputs;
    for (size_t i = 0; i < input_count; ++i) {
        // Name shenanigans.
        const auto name = tiny::Plug_io::resolve_audio_input_name(i, input_count);
        String128 tname{};
        StringConvert::convert(name, tname, 128);

        const auto bus_type = (i == 0) ? BusTypes::kMain : BusTypes::kAux;
        addAudioInput(tname, SpeakerArr::kStereo, bus_type);
    }

    const auto output_count = plug_io.audio_ports.num_outputs;
    for (size_t i = 0; i < output_count; ++i) {
        // Name shenanigans.
        const auto name = tiny::Plug_io::resolve_audio_output_name(i, output_count);
        String128 tname{};
        StringConvert::convert(name, tname, 128);

        const auto bus_type = (i == 0) ? BusTypes::kMain : BusTypes::kAux;
        addAudioOutput(tname, SpeakerArr::kStereo, bus_type);
    }

    // Create the event IO.
    

    return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API Processor::terminate()
{
    // Here the Plug-in will be de-instantiated, last possibility to remove some memory!

    // Do not forget to call parent.
    return Steinberg::Vst::AudioEffect::terminate();
}

Steinberg::tresult PLUGIN_API Processor::setActive(Steinberg::TBool state)
{
    // Called when the Plug-in is enable/disable (On/Off).
	return Steinberg::Vst::AudioEffect::setActive(state);
}

Steinberg::tresult PLUGIN_API Processor::setupProcessing(Steinberg::Vst::ProcessSetup& newSetup)
{
	// Called before any processing.
	return Steinberg::Vst::AudioEffect::setupProcessing(newSetup);
}

Steinberg::tresult PLUGIN_API Processor::canProcessSampleSize(Steinberg::int32 symbolicSampleSize)
{
	// By default kSample32 is supported.
	if (symbolicSampleSize == Steinberg::Vst::kSample32)
		return Steinberg::kResultTrue;

	return Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API Processor::process(Steinberg::Vst::ProcessData& /*data*/)
{
    return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API Processor::setState(Steinberg::IBStream* state)
{
    if (!state)
        return Steinberg::kResultFalse;

    return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API Processor::getState(Steinberg::IBStream* state)
{
    if (!state)
        return Steinberg::kResultFalse;

    return Steinberg::kResultOk;
}

} // namespace tiny

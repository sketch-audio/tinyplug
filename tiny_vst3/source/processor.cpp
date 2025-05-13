#include "processor.h"
#include "cids.h"

#include "base/source/fstreamer.h"

namespace tiny {

Processor::Processor()
{
    setControllerClass(controller_uid);
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
    addAudioInput(STR16("Stereo In"), Steinberg::Vst::SpeakerArr::kStereo);
    addAudioOutput(STR16("Stereo Out"), Steinberg::Vst::SpeakerArr::kStereo);

    // Create the event IO.
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

Steinberg::tresult PLUGIN_API Processor::process(Steinberg::Vst::ProcessData& data)
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

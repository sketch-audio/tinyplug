#include "pluginterfaces/base/ibstream.h"

#include "controller.h"

namespace tiny {

Steinberg::tresult PLUGIN_API Controller::initialize(Steinberg::FUnknown* context)
{
    // Here the plug-in will be instantiated.

    Steinberg::tresult result = Steinberg::Vst::EditControllerEx1::initialize(context);

    if (result != Steinberg::kResultOk)
        return result;

    // Here you could register some parameters.

    return result;
}

Steinberg::tresult PLUGIN_API Controller::terminate()
{
    // Here the Plug-in will be de-instantiated, last possibility to remove some memory!

    // Do not forget to call parent.
    return Steinberg::Vst::EditControllerEx1::terminate();
}

Steinberg::tresult PLUGIN_API Controller::setComponentState(Steinberg::IBStream* state)
{
	// Here you get the state of the component (processor part).
	if (!state)
		return Steinberg::kResultFalse;

	return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API Controller::setState(Steinberg::IBStream* state)
{
	// Here you get the state of the controller.

	return Steinberg::kResultTrue;
}

//------------------------------------------------------------------------
Steinberg::tresult PLUGIN_API Controller::getState(Steinberg::IBStream* state)
{
	// Here you are asked to deliver the state of the controller (if needed).
	// Note: the real state of your plug-in is saved in the processor.

	return Steinberg::kResultTrue;
}

//------------------------------------------------------------------------
Steinberg::IPlugView* PLUGIN_API Controller::createView(Steinberg::FIDString name)
{
	// Here the Host wants to open your editor (if you have one).
	if (Steinberg::FIDStringsEqual(name, Steinberg::Vst::ViewType::kEditor))
	{
		// Create your editor here and return a IPlugView ptr of it.
        return nullptr;
    }
    
    return nullptr;
}

Steinberg::tresult PLUGIN_API Controller::setParamNormalized(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue value)
{
	// Called by host to update your parameters.
	Steinberg::tresult result = Steinberg::Vst::EditControllerEx1::setParamNormalized(tag, value);
	return result;
}

//------------------------------------------------------------------------
Steinberg::tresult PLUGIN_API Controller::getParamStringByValue(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue valueNormalized, Steinberg::Vst::String128 string)
{
	// Called by host to get a string for given normalized value of a specific parameter.
	// (without having to set the value!)
	return Steinberg::Vst::EditControllerEx1::getParamStringByValue(tag, valueNormalized, string);
}

//------------------------------------------------------------------------
Steinberg::tresult PLUGIN_API Controller::getParamValueByString(Steinberg::Vst::ParamID tag, Steinberg::Vst::TChar* string, Steinberg::Vst::ParamValue& valueNormalized)
{
	// Called by host to get a normalized value from a string representation of a specific parameter.
	// (without having to set the value!)
	return Steinberg::Vst::EditControllerEx1::getParamValueByString(tag, string, valueNormalized);
}



} // namespace tiny
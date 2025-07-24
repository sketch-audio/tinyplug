#pragma once

#include "public.sdk/source/vst/vsteditcontroller.h"

#include "user/param_model.h"

#include "vst3_view.h"

class Vst3_controller : public Steinberg::Vst::EditControllerEx1
{
public:
    
    using Super = Steinberg::Vst::EditControllerEx1;
    Vst3_controller() = default;
    ~Vst3_controller() SMTG_OVERRIDE = default;

    // Create function
	static Steinberg::FUnknown* createInstance (void* /*context*/)
	{
		return (Steinberg::Vst::IEditController*)new Vst3_controller;
	}

	// IPluginBase
	Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) SMTG_OVERRIDE;
	Steinberg::tresult PLUGIN_API terminate() SMTG_OVERRIDE;

	// IEditController
	Steinberg::tresult PLUGIN_API setComponentState(Steinberg::IBStream* state) SMTG_OVERRIDE;
	Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* state) SMTG_OVERRIDE;
	Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* state) SMTG_OVERRIDE;
	Steinberg::tresult PLUGIN_API getParamStringByValue(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue valueNormalized, Steinberg::Vst::String128 string) SMTG_OVERRIDE;
	Steinberg::tresult PLUGIN_API getParamValueByString(Steinberg::Vst::ParamID tag, Steinberg::Vst::TChar* string, Steinberg::Vst::ParamValue& valueNormalized) SMTG_OVERRIDE;
	Steinberg::Vst::ParamValue PLUGIN_API normalizedParamToPlain(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue valueNormalized) SMTG_OVERRIDE;
	Steinberg::Vst::ParamValue PLUGIN_API plainParamToNormalized(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue plainValue) SMTG_OVERRIDE;
	Steinberg::Vst::ParamValue PLUGIN_API getParamNormalized(Steinberg::Vst::ParamID tag) SMTG_OVERRIDE;
	Steinberg::tresult PLUGIN_API setParamNormalized(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue value) SMTG_OVERRIDE;
	Steinberg::tresult PLUGIN_API setComponentHandler(Steinberg::Vst::IComponentHandler* handler) SMTG_OVERRIDE;
	Steinberg::IPlugView* PLUGIN_API createView(Steinberg::FIDString name) SMTG_OVERRIDE;

 	//---Interface---------
	DEFINE_INTERFACES
		// Here you can add more supported VST3 interfaces
		// DEF_INTERFACE (Vst::IXXX)
	END_DEFINE_INTERFACES (Super)
    DELEGATE_REFCOUNT(Super)

protected:

    Vst3_view* view{nullptr}; // Is there any point in keeping this around?

    // Sorted by paramId.
    using User_params = tiny::Params<tiny::Param_model>;
    User_params _params{};
    User_params::Param_values _uivalues{};

};
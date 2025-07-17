#pragma once

#include "AAX_CEffectParameters.h"

#include "user_plug.h"

class Aax_parameters : public AAX_CEffectParameters {
public:

    Aax_parameters() : AAX_CEffectParameters() {}
    ~Aax_parameters() override = default;

    static AAX_CEffectParameters* AAX_CALLBACK Create() { return new Aax_parameters; }

    AAX_Result EffectInit() override;

private:

    // Sorted by paramId.
    std::vector<tiny::Param_model::Spec> _specs{};
    //tiny::Param_model::Param_values _uivalues{};

};
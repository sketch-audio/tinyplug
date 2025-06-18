#pragma once

#include "AAX_CEffectParameters.h"
#include "AAX_CBinaryTaperDelegate.h"
#include "AAX_CBinaryDisplayDelegate.h"

class Aax_parameters : public AAX_CEffectParameters {
public:

    Aax_parameters() : AAX_CEffectParameters() {}
    ~Aax_parameters() override = default;

    static AAX_CEffectParameters* AAX_CALLBACK Create() { return new Aax_parameters; }

    AAX_Result EffectInit() override
    {
        // bypass
        AAX_CString bypassID = cDefaultMasterBypassID;
        AAX_IParameter * masterBypass = new AAX_CParameter<bool>(
            bypassID.CString(), AAX_CString("Master Bypass"), false,
            AAX_CBinaryTaperDelegate<bool>(),
            AAX_CBinaryDisplayDelegate<bool>("bypass", "on"), true);
        masterBypass->SetNumberOfSteps( 2 );
        masterBypass->SetType( AAX_eParameterType_Discrete );
        mParameterManager.AddParameter(masterBypass);

        mPacketDispatcher.RegisterPacket(bypassID.CString(), AAX_FIELD_INDEX(Aax_context, bypass));
        return AAX_SUCCESS;
    }

private:
    
};
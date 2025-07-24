#pragma once

#include <charconv>

#include "aax_monolith.h"

#include "plug_info.h"

#include "user/dsp_kernel.h"
#include "user/param_model.h"

class Aax_parameters : public tiny::aax::AAX_CMonolithicParameters {
public:

    using Super = tiny::aax::AAX_CMonolithicParameters;
    Aax_parameters() : Super() {}
    ~Aax_parameters() override = default;

    static AAX_CEffectParameters* AAX_CALLBACK Create() { return new Aax_parameters; }

    AAX_Result EffectInit() override;

    using RenderInfo = tiny::aax::AAX_SInstrumentRenderInfo;
    void RenderAudio(RenderInfo* ioRenderInfo, const TParamValPair* inSynchronizedParamValues[], int32_t inNumSynchronizedParamValues) override
    {
        //
        for (auto i = decltype(inNumSynchronizedParamValues){}; i < inNumSynchronizedParamValues; ++i) {
            const auto* sync_value = inSynchronizedParamValues[i];
            const auto aax_id = sync_value->first;
            const auto aax_param = sync_value->second;

            auto id = uint32_t{};
            std::from_chars(aax_id + 2, aax_id + std::strlen(aax_id), id, 16); // Should I do a map?

            // Obtain value
            auto value = double{};
            aax_param->GetValueAsDouble(&value);

            if (id < User_params::num_params) {
                _kernel->handle_event(tiny::Set_param{.id = id, .value = value});
            }
        }

        // Assign buffer ptrs.
        _ibuffers[0] = ioRenderInfo->mAudioInputs[0];
        _ibuffers[1] = ioRenderInfo->mAudioInputs[1];

        if constexpr (tiny::Plug_info::wants_sidechain) {
            _ibuffers[2] = ioRenderInfo->mAudioInputs[1];
        }

        _obuffers[0] = ioRenderInfo->mAudioOutputs[0];
        _obuffers[1] = ioRenderInfo->mAudioOutputs[1];

        // Process kernel.
        auto context = tiny::Dsp_context{
            .ibuffers = _ibuffers,
            .obuffers = _obuffers,
            .num_frames = static_cast<size_t>(*ioRenderInfo->mNumSamples)
        };
        _kernel->process(context);
    }

private:
    
    static constexpr auto num_ichannels = size_t{2 + (tiny::Plug_info::wants_sidechain ? 1 : 0)}; // AAX mono sidechain?
    static constexpr auto num_ochannels = size_t{2};

    // Pointers to host io buffers.
    std::array<const float*, num_ichannels> _ibuffers{};
    std::array<float*, num_ochannels> _obuffers{};

    using User_params = tiny::Params<tiny::Param_model>;
    User_params _params{};

    std::unique_ptr<tiny::Dsp_kernel> _kernel = std::make_unique<tiny::Dsp_kernel>();

};
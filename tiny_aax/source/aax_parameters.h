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
        using namespace tiny;

        for (auto i = decltype(inNumSynchronizedParamValues){}; i < inNumSynchronizedParamValues; ++i) {
            const auto* sync_value = inSynchronizedParamValues[i];
            const auto aax_id = sync_value->first;
            const auto aax_param = sync_value->second;

            auto id = uint32_t{};
            // +2 gets rid of "0x"
            std::from_chars(aax_id + 2, aax_id + std::strlen(aax_id), id, 16); // Should I do a map?

            // Obtain value
            auto value = double{};
            aax_param->GetValueAsDouble(&value);

            if (id < User_params::num_params) {
                _kernel->handle_event(Set_param{.id = id, .value = value});
            }
        }

        // Assign buffer ptrs.
        for (size_t i = 0; i < num_ichannels; ++i) {
            _ibuffers[i] = ioRenderInfo->mAudioInputs[i];
        }
        for (size_t i = 0; i < num_ochannels; ++i) {
            _obuffers[i] = ioRenderInfo->mAudioOutputs[i];
        }
        if constexpr (Plug_info::wants_sidechain) {
            if (const auto sc_idx = ioRenderInfo->mSidechainIndex; sc_idx != nullptr) {
                for (size_t i = 0; i < num_schannels; ++i) {
                    _sbuffers[i] = ioRenderInfo->mAudioInputs[*sc_idx + i];
                }
            }
        }

        // Process kernel.
        auto context = tiny::Dsp_context{
            .ibuffers = _ibuffers,
            .sbuffers = _sbuffers,
            .obuffers = _obuffers,
            .num_frames = static_cast<size_t>(*ioRenderInfo->mNumSamples),
            .exports = _exports
        };
        _kernel->process(context);

        // Write exports to meters.
        for (size_t i = 0; i < num_exports; ++i) {
            if (context.exports[i] != _lexports[i]) {
                // Send an output event.
                _oqueue.push({
                    .id = static_cast<uint32_t>(i),
                    .value = context.exports[i]
                });

                // Cache for next time.
                _lexports[i] = context.exports[i];
            }

            _exports[i] = 0; // Reset for peak meters.
        }
    }

    auto pop_export(tiny::Export_event& event) -> bool
    {
        return _oqueue.pop(event);
    }

private:

    using User_params = tiny::Params<tiny::Param_model>;
    using User_exports = tiny::Exports<tiny::Param_model>;

    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_exports = User_exports::num_exports;

    static constexpr auto num_ichannels = size_t{2};
    static constexpr auto num_schannels = size_t{1}; // mono sidechain? verify.
    static constexpr auto num_ochannels = size_t{2};

    // Pointers to host io buffers.
    std::array<const float*, num_ichannels> _ibuffers{};
    std::array<const float*, num_schannels> _sbuffers{};
    std::array<float*, num_ochannels> _obuffers{};
    std::array<float, num_exports> _exports{};

    std::array<double, num_exports> _lexports{};

    User_params _params{};

    using Export_queue = tiny::Lock_free_queue<tiny::Export_event, 256>;
    Export_queue _oqueue{};

    std::unique_ptr<tiny::Dsp_kernel> _kernel = std::make_unique<tiny::Dsp_kernel>();

};
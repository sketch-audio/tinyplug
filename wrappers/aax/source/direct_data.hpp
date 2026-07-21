#pragma once

#include <array>
#include <cstdint>

#include "AAX_CEffectDirectData.h"

#include "alg_context.hpp"

namespace tiny::aax {

/*
    The return channel.

    The algorithm has no route back to the data model — by design. Everything flowing
    outwards (meters, worker messages, latency proposals) is staged in a ring inside
    the algorithm's private data and copied across here, on the Direct Data timer,
    via AAX_IPrivateDataAccess::ReadPortDirect — a memcpy of a byte range, which is
    the AAX analogue of a VST3 IMessage.

    This object is a dumb pipe: it frames and forwards, and lets the data model decide
    what each message means. The wakeup is roughly every 30 ms and is not guaranteed
    to be regular, so nothing here may assume a rate.
*/
class Direct_data : public AAX_CEffectDirectData {
public:

    static AAX_IEffectDirectData* AAX_CALLBACK Create() { return new Direct_data; }

    AAX_Result TimerWakeup_PrivateDataAccess(AAX_IPrivateDataAccess* private_data) override;

private:

    auto _drain_returns(AAX_IPrivateDataAccess* access) -> void;
    auto _push_worker_replies(AAX_IPrivateDataAccess* access) -> void;

    // Bounds how much we lift out of the return ring per wakeup. A partial entry at
    // the tail is simply left for the next pass.
    static constexpr auto scratch_bytes = size_t{8192};
    std::array<unsigned char, scratch_bytes> _scratch{};

};

} // namespace tiny::aax

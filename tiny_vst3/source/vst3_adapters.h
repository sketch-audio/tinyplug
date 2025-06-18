#pragma once

#include "pluginterfaces/base/funknown.h"

#include "tinyplug.h"

namespace tiny {
inline auto map_to_fuid(const Plug_info::Vst3_uid& uid) -> Steinberg::FUID
{
    return {uid[0], uid[1], uid[2], uid[3]};
}
} // namespace tiny
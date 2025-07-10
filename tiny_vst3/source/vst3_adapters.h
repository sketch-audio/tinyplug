#pragma once

#include "pluginterfaces/base/funknown.h"

#include "cmake_defines.h"

namespace tiny {

using Uid_arr = Cmake_defines::Vst3::Uid_arr;

inline auto map_to_fuid(const Uid_arr& uid) -> Steinberg::FUID
{
    return {uid[0], uid[1], uid[2], uid[3]};
}

} // namespace tiny
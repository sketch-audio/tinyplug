#pragma once

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/vsttypes.h"

namespace tiny {

static const Steinberg::FUID controller_uid{'Skch', 'tiny', 'ctrl', 0};
static const Steinberg::FUID processor_uid{'Skch', 'tiny', 'proc', 0};

} // namespace tiny
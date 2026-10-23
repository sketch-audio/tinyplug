#include "processor.hpp"

#include <algorithm>
#include <cstring>

namespace tiny::process {

auto Processor::process(Context& context) -> void
{
    const auto channels = std::min(context.inputs.size(), context.outputs.size());
    for (auto channel = size_t{}; channel < channels; ++channel) {
        if (context.outputs[channel] != context.inputs[channel]) {
            std::memcpy(context.outputs[channel], context.inputs[channel], context.num_frames * sizeof(float));
        }
    }
    for (auto channel = channels; channel < context.outputs.size(); ++channel) {
        std::memset(context.outputs[channel], 0, context.num_frames * sizeof(float));
    }
}

} // namespace tiny::process

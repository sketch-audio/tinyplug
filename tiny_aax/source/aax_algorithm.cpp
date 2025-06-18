#include "aax_algorithm.h"

void AAX_CALLBACK Aax_algorithm(Aax_context* const instances_begin[], const void* const instances_end)
{
    for (auto it = instances_begin; it < instances_end; ++it) {
        auto* instance = *it;

        const auto bypass = *instance->bypass;
        const auto buffer_size = *instance->buffer_size;

        for (size_t i = 0; i < buffer_size; ++i) {

        }
    }
}
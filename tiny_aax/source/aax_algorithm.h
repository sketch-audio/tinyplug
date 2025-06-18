#pragma once

#include "AAX.h"

const AAX_CTypeID cDemoGain_MeterID[2] = {'mtrI','mtrO'};

class Aax_parameters;

struct Aax_context {
    int32_t* bypass;
    float** input_channels;
    float** output_channels;
    int32_t* buffer_size;
    //float** meter_taps;
    Aax_parameters* plugin;
};

void AAX_CALLBACK Aax_algorithm(Aax_context* const [], const void* const);
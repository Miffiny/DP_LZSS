#pragma once

#include "parser.h"
#include "tans.h"

struct LzssAdaptiveAcCostModel;

bool lzss_encode_optimal(
    const uint8_t *input,
    size_t input_size,
    const LzssConfig *config,
    const LzssTansCostModel *cost_model,
    LzssSequenceStream *out_stream);

bool lzss_encode_optimal_adaptive_ac(
    const uint8_t *input,
    size_t input_size,
    const LzssConfig *config,
    const LzssAdaptiveAcCostModel *cost_model,
    LzssSequenceStream *out_stream);

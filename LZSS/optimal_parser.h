#pragma once

#include "parser.h"
#include "tans.h"

bool lzss_encode_optimal(
    const uint8_t *input,
    size_t input_size,
    const LzssConfig *config,
    const LzssTansCostModel *cost_model,
    LzssSequenceStream *out_stream);

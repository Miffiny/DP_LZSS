#pragma once

#include <cstdint>
#include <cstddef>
#include "token.h"

//Future usage
typedef enum {
    LZSS_PARSE_GREEDY,
    LZSS_PARSE_LAZY,
    LZSS_PARSE_COST_AWARE
} LzssParseMode;

typedef struct {
    size_t window_size;
    size_t min_match_length;
    size_t max_match_length;
    //TODO + need to add some experiments with it
    //bool use_lazy_match;

    //TODO for future use
    //LzssParseMode parse_mode;
    //size_t max_candidates;
} LzssConfig;

bool lzss_encode(const uint8_t* input, size_t input_size,
                 const LzssConfig* config, LzssTokenStream* out_stream);
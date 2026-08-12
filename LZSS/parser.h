#pragma once

#include <cstdint>
#include <cstddef>
#include "token.h"

typedef enum {
    LZSS_PARSE_GREEDY,
    LZSS_PARSE_LAZY,
    LZSS_PARSE_OPTIMAL
    // Future parser work can add a cost-aware mode here.
} LzssParseMode;

typedef enum {
    LZSS_HASH3,
    LZSS_HASH4
} LzssHashMode;

typedef struct {
    size_t window_size;
    size_t min_match_length;
    size_t max_match_length;
    LzssParseMode parse_mode;
    LzssHashMode hash_mode;
    //size_t max_candidates;
} LzssConfig;

bool lzss_encode(const uint8_t* input, size_t input_size,
                 const LzssConfig* config, LzssSequenceStream* out_stream);

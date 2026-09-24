#pragma once

#include <cstdint>
#include <cstddef>
#include "token.h"

typedef enum {
    LZSS_PARSE_GREEDY,
    LZSS_PARSE_LAZY,
    LZSS_PARSE_OPTIMAL,
    LZSS_PARSE_ADAPTIVE_OPTIMAL
    // Future parser work can add a cost-aware mode here.
} LzssParseMode;

typedef enum {
    LZSS_HASH3,
    LZSS_HASH4
} LzssHashMode;

typedef enum {
    LZSS_DISTANCE_CLASS,
    LZSS_DISTANCE_BIT_TREE
} LzssDistanceCodingMode;

static constexpr size_t LZSS_DEFAULT_MAX_CHAIN_LENGTH = 256;
static constexpr size_t LZSS_DEFAULT_GOOD_MATCH_LENGTH = 32;
static constexpr size_t LZSS_DEFAULT_OPTIMAL_MAX_CHAIN_LENGTH = 1024;
static constexpr size_t LZSS_DEFAULT_HASH_SIZE = 1u << 17;
static constexpr size_t LZSS_DEFAULT_TANS_TABLE_LOG = 12;

typedef struct {
    size_t window_size;
    size_t min_match_length;
    size_t max_match_length;
    size_t max_chain_length;
    size_t good_match_length;
    size_t optimal_max_chain_length;
    size_t hash_size;
    size_t tans_table_log;
    LzssParseMode parse_mode;
    LzssHashMode hash_mode;
    LzssDistanceCodingMode distance_coding;
    //size_t max_candidates;
} LzssConfig;

bool lzss_encode(const uint8_t* input, size_t input_size,
                 const LzssConfig* config, LzssSequenceStream* out_stream);

#pragma once

#include "LZSS/decoder.h"
#include "LZSS/parser.h"

#include <cstddef>
#include <cstdint>
#include <vector>

static constexpr size_t LZSS_TANS_DEFAULT_BLOCK_SIZE = 8u * 1024u * 1024u;
static constexpr size_t LZSS_TANS_DEFAULT_MAX_WORKERS = 8;

enum LzssTansTableMode {
    LZSS_TANS_TABLE_LAZY,
    LZSS_TANS_TABLE_REBUILD
};

struct LzssBlockStats {
    size_t token_count = 0;
    size_t match_token_count = 0;
    size_t literal_token_count = 0;
    size_t match_memory = 0;
    size_t match_length_total = 0;
    size_t rep0_count = 0;
    size_t rep1_count = 0;
    size_t rep2_count = 0;
    size_t new_distance_count = 0;
};

struct LzssPayloadStats {
    size_t exact_payload_bits = 0;
    size_t rounded_payload_bytes = 0;
    size_t model_header_bits = 0;
    size_t tans_stream_bits = 0;
    size_t extra_stream_bits = 0;
    size_t padding_bits = 0;
    size_t total_stream_bytes = 0;
    double empirical_entropy_bits = 0.0;
    double normalization_loss_bits = 0.0;
    double tans_coder_overhead_bits = 0.0;
};

struct LzssBlockTimings {
    double parse_ms = 0.0;
    double model_build_ms = 0.0;
    double entropy_encode_ms = 0.0;
    double entropy_decode_ms = 0.0;
    double reconstruct_ms = 0.0;
};

struct LzssTansBlock {
    std::vector<uint32_t> compressed_words;
};

struct LzssTansBlockStream {
    size_t original_size;
    size_t block_size;
    std::vector<LzssTansBlock> blocks;
    LzssBlockStats stats;
    LzssPayloadStats payload_stats;
    LzssBlockTimings timings;
};

void lzss_tans_block_stream_init(LzssTansBlockStream *stream);
void lzss_tans_block_stream_clear(LzssTansBlockStream *stream);

size_t lzss_tans_block_stream_compressed_size(
    const LzssTansBlockStream *stream);

bool lzss_tans_encode_blocks(
    const uint8_t *input,
    size_t input_size,
    size_t block_size,
    size_t max_workers,
    const LzssConfig *config,
    LzssTansTableMode table_mode,
    LzssTansBlockStream *out_stream);

bool lzss_tans_decode_blocks(
    const LzssTansBlockStream *stream,
    size_t max_workers,
    const LzssConfig *config,
    ByteBuffer *out,
    LzssBlockTimings *timings = nullptr);

#pragma once

#include "LZSS/decoder.h"
#include "LZSS/parser.h"

#include <cstddef>
#include <cstdint>
#include <vector>

static constexpr size_t LZSS_TANS_BLOCK_SIZE = 8u * 1024u * 1024u;

struct LzssBlockStats {
    size_t token_count;
    size_t match_token_count;
    size_t literal_token_count;
    size_t match_memory;
    size_t match_length_total;
};

struct LzssTansBlock {
    std::vector<uint32_t> compressed_words;
};

struct LzssTansBlockStream {
    size_t original_size;
    size_t block_size;
    std::vector<LzssTansBlock> blocks;
    LzssBlockStats stats;
};

void lzss_tans_block_stream_init(LzssTansBlockStream *stream);
void lzss_tans_block_stream_clear(LzssTansBlockStream *stream);

size_t lzss_tans_block_stream_compressed_size(
    const LzssTansBlockStream *stream);

bool lzss_tans_encode_blocks(
    const uint8_t *input,
    size_t input_size,
    const LzssConfig *config,
    LzssTansBlockStream *out_stream);

bool lzss_tans_decode_blocks(
    const LzssTansBlockStream *stream,
    const LzssConfig *config,
    ByteBuffer *out);

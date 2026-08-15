#pragma once

#include "LZSS/decoder.h"
#include "LZSS/parser.h"
#include "block_tans.h"

#include <cstddef>
#include <cstdint>
#include <vector>

struct LzssAcBlock {
    std::vector<uint32_t> compressed_words;
};

struct LzssAcBlockStream {
    size_t original_size;
    size_t block_size;
    std::vector<LzssAcBlock> blocks;
    LzssBlockStats stats;
};

void lzss_ac_block_stream_init(LzssAcBlockStream *stream);
void lzss_ac_block_stream_clear(LzssAcBlockStream *stream);

size_t lzss_ac_block_stream_compressed_size(
    const LzssAcBlockStream *stream);

bool lzss_ac_encode_blocks(
    const uint8_t *input,
    size_t input_size,
    size_t block_size,
    size_t max_workers,
    const LzssConfig *config,
    LzssAcBlockStream *out_stream);

bool lzss_ac_decode_blocks(
    const LzssAcBlockStream *stream,
    size_t max_workers,
    const LzssConfig *config,
    ByteBuffer *out);

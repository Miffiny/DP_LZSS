#pragma once

#include <cstddef>
#include <cstdint>

#include "ac.h"
#include "bio.h"
#include "parser.h"
#include "token.h"

struct LzssArithmeticCodec {
    LzssConfig config;

    // Number of bits for distance - 1
    size_t distance_bit_count;

    struct model event_model;    // LITERAL / MATCH / EOF
    struct model literal_model;  // 0..255
    struct model length_model;   // length - min_match_length
    struct model *distance_bit_models;
};

bool lzss_ac_codec_init(
    LzssArithmeticCodec *codec,
    const LzssConfig *config
);

void lzss_ac_codec_destroy(
    LzssArithmeticCodec *codec
);

bool lzss_ac_encode_stream(
    LzssArithmeticCodec *codec,
    struct ac *ac,
    struct bio *bio,
    const LzssTokenStream *stream
);

bool lzss_ac_decode_stream(
    LzssArithmeticCodec *codec,
    struct ac *ac,
    struct bio *bio,
    LzssTokenStream *out_stream
);
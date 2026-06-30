#pragma once

#include <cstddef>
#include <cstdint>

#include "ac.h"
#include "bio.h"
#include "parser.h"
#include "token.h"

struct LzssArithmeticCodec {
    LzssConfig config;

    size_t length_extra_bit_count;
    size_t distance_extra_bit_count;

    struct model event_model;    // LITERAL / MATCH / EOF
    struct model literal_model;  // 0..255
    struct model length_model;   // Deflate-like length class
    struct model distance_model; // Deflate-like distance class
    struct model *length_extra_bit_models;
    struct model *distance_extra_bit_models;
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

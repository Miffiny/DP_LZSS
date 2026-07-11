#pragma once

#include "arithmetic.h"

bool lzss_ac_encode_stream_dynamic(
    LzssArithmeticCodec *codec,
    struct ac *ac,
    struct bio *bio,
    const LzssTokenStream *stream
);

bool lzss_ac_decode_stream_dynamic(
    LzssArithmeticCodec *codec,
    struct ac *ac,
    struct bio *bio,
    LzssTokenStream *out_stream
);

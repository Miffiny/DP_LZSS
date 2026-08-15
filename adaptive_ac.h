#pragma once

#include "ac.h"
#include "LZSS/decoder.h"
#include "LZSS/parser.h"

#include <cstddef>
#include <cstdint>

struct LzssAdaptiveAcCodec {
    LzssConfig config;

    size_t literal_length_extra_bit_count;
    size_t length_extra_bit_count;
    size_t distance_extra_bit_count;

    size_t literal_length_context_count;
    size_t literal_context_count;
    size_t length_context_count;
    size_t distance_context_count;

    struct model *literal_length_models;
    struct model *literal_models;
    struct model *length_models;
    struct model *distance_models;

    struct model *literal_length_extra_bit_models;
    struct model *length_extra_bit_models;
    struct model *distance_extra_bit_models;
};

bool lzss_adaptive_ac_codec_init(
    LzssAdaptiveAcCodec *codec,
    const LzssConfig *config);

void lzss_adaptive_ac_codec_destroy(
    LzssAdaptiveAcCodec *codec);

bool lzss_adaptive_ac_encode_stream(
    LzssAdaptiveAcCodec *codec,
    struct ac *ac,
    struct bio *bio,
    const LzssSequenceStream *stream);

bool lzss_adaptive_ac_decode_stream(
    LzssAdaptiveAcCodec *codec,
    struct ac *ac,
    struct bio *bio,
    LzssSequenceStream *out_stream);

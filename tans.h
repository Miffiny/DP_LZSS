#ifndef TANS_H
#define TANS_H

#include <stdint.h>
#include <stddef.h>
#include "bio.h"
#include "ac.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TANS_L 4096

struct tans_decode_entry {
    uint16_t new_x;
    uint16_t symbol;
    uint8_t num_bits;
};

struct tans_model {
    struct tans_decode_entry decode_table[TANS_L];

    uint16_t* symbol_cum;
    uint16_t* symbol_freqs;
    uint32_t count;
};

struct tans_state {
    uint32_t x;
};

int tans_model_init(struct tans_model *tm, const struct model *m);
void tans_model_destroy(struct tans_model *tm);

void tans_encode_init(struct tans_state *ts);
void tans_encode_flush(struct tans_state *ts, struct bio *bio);

void tans_decode_init(struct tans_state *ts, struct bio *bio);
size_t tans_decode_symbol(struct tans_state *ts, struct bio *bio, const struct tans_model *tm);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

#include "parser.h"
#include "token.h"

#include <vector>

struct LzssTansCodec {
    LzssConfig config;

    size_t literal_length_extra_bit_count;
    size_t length_extra_bit_count;
    size_t distance_extra_bit_count;

    struct model literal_length_model;
    struct model literal_model;
    struct model length_model;
    struct model distance_model;

    struct tans_model literal_length_tans_model;
    struct tans_model literal_tans_model;
    struct tans_model length_tans_model;
    struct tans_model distance_tans_model;

    bool literal_length_tans_ready;
    bool literal_tans_ready;
    bool length_tans_ready;
    bool distance_tans_ready;

    double empirical_entropy_bits;
    double normalized_model_bits;
};

struct LzssTansStreamStats {
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

struct LzssTansCostModel {
    std::vector<double> literal_costs;
    std::vector<double> literal_length_costs;
    std::vector<double> length_costs;
    std::vector<double> distance_symbol_costs;
    std::vector<double> distance_costs;
    double match_sequence_cost;
};

struct LzssRepeatDistanceState {
    uint32_t distances[3] = {0, 0, 0};
};

bool lzss_tans_codec_init(
    LzssTansCodec *codec,
    const LzssConfig *config
);

void lzss_tans_codec_destroy(
    LzssTansCodec *codec
);

bool lzss_tans_encode_stream(
    LzssTansCodec *codec,
    struct bio *bio,
    const LzssSequenceStream *stream,
    LzssTansStreamStats *stats = nullptr
);

bool lzss_tans_build_models(
    LzssTansCodec *codec,
    const LzssSequenceStream *stream,
    bool smooth_all_symbols
);

bool lzss_tans_encode_stream_with_current_models(
    LzssTansCodec *codec,
    struct bio *bio,
    const LzssSequenceStream *stream,
    LzssTansStreamStats *stats = nullptr
);

bool lzss_tans_cost_model_init(
    const LzssTansCodec *codec,
    LzssTansCostModel *cost_model
);

void lzss_repeat_distance_state_init(
    LzssRepeatDistanceState *state
);

void lzss_repeat_distance_state_update(
    LzssRepeatDistanceState *state,
    uint32_t distance
);

bool lzss_tans_literal_length_cost(
    const LzssTansCostModel *cost_model,
    size_t literal_length,
    double *out_cost
);

bool lzss_tans_match_cost(
    const LzssTansCostModel *cost_model,
    const LzssRepeatDistanceState *repeat_state,
    uint32_t match_length,
    uint32_t match_distance,
    double *out_cost,
    LzssRepeatDistanceState *out_repeat_state
);

bool lzss_tans_decode_stream(
    LzssTansCodec *codec,
    struct bio *bio,
    LzssSequenceStream *out_stream
);

#endif

#endif

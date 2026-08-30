#pragma once

#include "ac.h"
#include "LZSS/decoder.h"
#include "LZSS/parser.h"

#include <cstddef>
#include <cstdint>
#include <vector>

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

    size_t distance_bit_count;
    void *distance_bit_tree_models;
};

struct LzssAdaptiveAcCostModel {
    LzssConfig config;

    size_t literal_length_symbol_count;
    size_t length_symbol_count;
    size_t distance_symbol_count;

    size_t literal_length_context_count;
    size_t literal_context_count;
    size_t length_context_count;
    size_t distance_context_count;

    size_t distance_bit_count;

    std::vector<double> literal_length_symbol_costs;
    std::vector<double> literal_costs;
    std::vector<double> length_symbol_costs;
    std::vector<double> distance_symbol_costs;
    std::vector<double> distance_bit_costs;
};

struct LzssAdaptiveAcCostState {
    uint32_t repeat_distances[3];

    size_t literal_length_base_context;
    size_t literal_context;
    size_t length_context;
    size_t distance_context;
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

bool lzss_adaptive_ac_cost_model_init(
    const LzssConfig *config,
    const LzssSequenceStream *seed_stream,
    LzssAdaptiveAcCostModel *cost_model);

void lzss_adaptive_ac_cost_state_init(
    LzssAdaptiveAcCostState *state);

bool lzss_adaptive_ac_literal_length_cost(
    const LzssAdaptiveAcCostModel *cost_model,
    size_t literal_length_base_context,
    size_t literal_length,
    double *out_cost);

bool lzss_adaptive_ac_literal_transition_cost(
    const LzssAdaptiveAcCostModel *cost_model,
    const LzssAdaptiveAcCostState *state,
    size_t literal_run_length,
    uint8_t literal,
    double *out_delta_cost,
    LzssAdaptiveAcCostState *out_state);

bool lzss_adaptive_ac_match_transition_cost(
    const LzssAdaptiveAcCostModel *cost_model,
    const LzssAdaptiveAcCostState *state,
    size_t literal_run_length,
    uint32_t match_length,
    uint32_t match_distance,
    double *out_delta_cost,
    LzssAdaptiveAcCostState *out_state);

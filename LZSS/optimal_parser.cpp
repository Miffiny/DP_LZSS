#include "optimal_parser.h"

#include "mf.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

struct OptimalParent {
    size_t prev_pos;
    LzssMatch match;
    LzssRepeatDistanceState rep_state;
    size_t literal_run_length;
    bool is_match;
    bool reachable;
};

struct OptimalAction {
    LzssMatch match;
    bool is_match;
};

static bool fits_u32(size_t value)
{
    return value <= std::numeric_limits<uint32_t>::max();
}

static bool emit_sequence(LzssSequenceStream *out_stream,
                          const LzssMatch *match,
                          const uint8_t *literals_ptr,
                          size_t lit_length)
{
    if (!fits_u32(lit_length)) {
        return false;
    }

    LzssSequence sequence{};
    if (match != nullptr) {
        sequence.match_distance = match->distance;
        sequence.match_length = match->length;
    }
    sequence.lit_length = static_cast<uint32_t>(lit_length);
    sequence.literals_ptr = lit_length > 0 ? literals_ptr : nullptr;

    sequence_stream_push(out_stream, sequence);
    return true;
}

static double literal_cost(
    const LzssTansCostModel *cost_model,
    uint8_t literal)
{
    return cost_model->literal_costs[literal];
}

static bool literal_length_cost(
    const LzssTansCostModel *cost_model,
    size_t literal_length,
    double *out_cost)
{
    return lzss_tans_literal_length_cost(
        cost_model,
        literal_length,
        out_cost
    );
}

static bool build_sequence_stream_from_actions(
    const uint8_t *input,
    const std::vector<OptimalAction>& actions,
    LzssSequenceStream *out_stream)
{
    size_t pos = 0;
    size_t lit_start_pos = 0;
    bool has_pending_match = false;
    LzssMatch pending_match{};

    for (const OptimalAction& action : actions) {
        if (!action.is_match) {
            ++pos;
            continue;
        }

        const size_t lit_length = pos - lit_start_pos;
        if (!emit_sequence(
                out_stream,
                has_pending_match ? &pending_match : nullptr,
                input + lit_start_pos,
                lit_length)) {
            return false;
        }

        pos += action.match.length;
        pending_match = action.match;
        has_pending_match = true;
        lit_start_pos = pos;
    }

    const size_t final_lit_length = pos - lit_start_pos;
    if (has_pending_match || final_lit_length > 0) {
        if (!emit_sequence(
                out_stream,
                has_pending_match ? &pending_match : nullptr,
                input + lit_start_pos,
                final_lit_length)) {
            return false;
        }
    }

    return true;
}

bool lzss_encode_optimal(
    const uint8_t *input,
    size_t input_size,
    const LzssConfig *config,
    const LzssTansCostModel *cost_model,
    LzssSequenceStream *out_stream)
{
    if ((input_size > 0 && input == nullptr) ||
        config == nullptr ||
        cost_model == nullptr ||
        out_stream == nullptr ||
        config->window_size == 0 ||
        config->min_match_length == 0 ||
        config->max_match_length < config->min_match_length) {
        return false;
    }

    if (input_size == 0) {
        return true;
    }

    if (cost_model->literal_costs.size() < 256 ||
        cost_model->literal_length_costs.empty() ||
        cost_model->length_costs.size() <= config->max_match_length ||
        cost_model->distance_symbol_costs.empty() ||
        cost_model->distance_costs.size() <= config->window_size) {
        return false;
    }

    LzssMatchFinder *match_finder =
        match_finder_create(config->window_size, config->hash_mode);
    if (match_finder == nullptr) {
        return false;
    }

    const double infinity = std::numeric_limits<double>::infinity();
    std::vector<double> costs(input_size + 1, infinity);
    std::vector<OptimalParent> parents(input_size + 1);
    std::vector<LzssMatch> matches;

    double empty_literal_run_cost = 0.0;
    if (!literal_length_cost(cost_model, 0, &empty_literal_run_cost)) {
        match_finder_destroy(match_finder);
        return false;
    }

    costs[0] = empty_literal_run_cost;
    parents[0].reachable = true;
    lzss_repeat_distance_state_init(&parents[0].rep_state);
    parents[0].literal_run_length = 0;

    for (size_t pos = 0; pos < input_size; ++pos) {
        if (std::isfinite(costs[pos])) {
            const double current_cost = costs[pos];
            const OptimalParent& current_parent = parents[pos];
            const double current_literal_cost =
                literal_cost(cost_model, input[pos]);

            double old_literal_length_cost = 0.0;
            double new_literal_length_cost = 0.0;
            const size_t new_literal_run_length =
                current_parent.literal_run_length + 1;

            if (std::isfinite(current_literal_cost) &&
                literal_length_cost(
                    cost_model,
                    current_parent.literal_run_length,
                    &old_literal_length_cost) &&
                literal_length_cost(
                    cost_model,
                    new_literal_run_length,
                    &new_literal_length_cost)) {
                const double next_cost =
                    current_cost +
                    current_literal_cost +
                    new_literal_length_cost -
                    old_literal_length_cost;

                if (next_cost < costs[pos + 1]) {
                    costs[pos + 1] = next_cost;
                    parents[pos + 1].prev_pos = pos;
                    parents[pos + 1].match = {};
                    parents[pos + 1].rep_state = current_parent.rep_state;
                    parents[pos + 1].literal_run_length =
                        new_literal_run_length;
                    parents[pos + 1].is_match = false;
                    parents[pos + 1].reachable = true;
                }
            }

            if (!match_finder_get_matches(
                    match_finder,
                    input,
                    pos,
                    input_size,
                    config->min_match_length,
                    config->max_match_length,
                    &matches)) {
                match_finder_destroy(match_finder);
                return false;
            }

            for (const LzssMatch& match : matches) {
                const size_t next_pos = pos + match.length;
                if (next_pos > input_size) {
                    continue;
                }

                double current_match_cost = 0.0;
                LzssRepeatDistanceState next_repeat_state{};
                if (!lzss_tans_match_cost(
                        cost_model,
                        &current_parent.rep_state,
                        match.length,
                        match.distance,
                        &current_match_cost,
                        &next_repeat_state)) {
                    continue;
                }

                const double next_cost =
                    current_cost + current_match_cost + empty_literal_run_cost;

                if (next_cost < costs[next_pos]) {
                    costs[next_pos] = next_cost;
                    parents[next_pos].prev_pos = pos;
                    parents[next_pos].match = match;
                    parents[next_pos].rep_state = next_repeat_state;
                    parents[next_pos].literal_run_length = 0;
                    parents[next_pos].is_match = true;
                    parents[next_pos].reachable = true;
                }
            }
        }

        match_finder_insert_position(match_finder, input, pos, input_size);
    }

    match_finder_destroy(match_finder);

    if (!parents[input_size].reachable) {
        return false;
    }

    std::vector<OptimalAction> actions;
    for (size_t pos = input_size; pos > 0;) {
        const OptimalParent& parent = parents[pos];
        if (!parent.reachable || parent.prev_pos >= pos) {
            return false;
        }

        OptimalAction action{};
        action.is_match = parent.is_match;
        action.match = parent.match;
        actions.push_back(action);
        pos = parent.prev_pos;
    }

    std::reverse(actions.begin(), actions.end());
    return build_sequence_stream_from_actions(input, actions, out_stream);
}

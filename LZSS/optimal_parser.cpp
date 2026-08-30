#include "optimal_parser.h"

#include "adaptive_ac.h"
#include "mf.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
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

struct AdaptiveOptimalParent {
    double cost;
    size_t prev_pos;
    size_t prev_slot;
    LzssMatch match;
    LzssAdaptiveAcCostState cost_state;
    size_t literal_run_length;
    bool is_match;
    bool reachable;
};

static constexpr size_t ADAPTIVE_OPTIMAL_BEAM_WIDTH = 2;

static const uint32_t REPEAT_LENGTH_CANDIDATE_POINTS[] = {
    3, 4, 5, 6, 7, 8, 9, 10,
    12, 14, 16, 18,
    22, 26, 30, 34,
    42, 50, 58, 66,
    82, 98, 114, 130,
    162, 194, 226, 257, 258
};

static uint64_t load_u64(const uint8_t *data)
{
    uint64_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

static size_t count_candidate_match_length(
    const uint8_t *input,
    size_t input_size,
    size_t pos,
    size_t distance,
    size_t max_length)
{
    if (input == nullptr || distance == 0 || distance > pos) {
        return 0;
    }

    const size_t max_possible_length =
        std::min(max_length, input_size - pos);
    const size_t match_pos = pos - distance;
    size_t length = 0;

    while (length + sizeof(uint64_t) <= max_possible_length &&
           load_u64(input + match_pos + length) ==
               load_u64(input + pos + length)) {
        length += sizeof(uint64_t);
    }

    while (length < max_possible_length &&
           input[match_pos + length] == input[pos + length]) {
        ++length;
    }

    return length;
}

static bool match_candidate_exists(
    const std::vector<LzssMatch>& matches,
    uint32_t length,
    uint32_t distance)
{
    for (const LzssMatch& match : matches) {
        if (match.length == length && match.distance == distance) {
            return true;
        }
    }

    return false;
}

static void append_match_candidate_if_new(
    std::vector<LzssMatch> *matches,
    uint32_t length,
    uint32_t distance)
{
    if (!match_candidate_exists(*matches, length, distance)) {
        LzssMatch match{};
        match.length = length;
        match.distance = distance;
        matches->push_back(match);
    }
}

static bool append_repeat_distance_candidates(
    const uint8_t *input,
    size_t input_size,
    size_t pos,
    const LzssConfig *config,
    const LzssAdaptiveAcCostState *state,
    std::vector<LzssMatch> *matches)
{
    if ((input_size > 0 && input == nullptr) ||
        config == nullptr ||
        state == nullptr ||
        matches == nullptr) {
        return false;
    }

    for (const uint32_t distance : state->repeat_distances) {
        if (distance == 0 ||
            distance > config->window_size ||
            distance > pos ||
            input_size - pos < config->min_match_length) {
            continue;
        }

        const size_t length = count_candidate_match_length(
            input,
            input_size,
            pos,
            distance,
            config->max_match_length
        );

        if (length < config->min_match_length) {
            continue;
        }

        for (const uint32_t candidate_length :
             REPEAT_LENGTH_CANDIDATE_POINTS) {
            if (candidate_length >= config->min_match_length &&
                candidate_length <= length) {
                append_match_candidate_if_new(
                    matches,
                    candidate_length,
                    distance
                );
            }
        }

        append_match_candidate_if_new(
            matches,
            static_cast<uint32_t>(length),
            distance
        );
    }

    return true;
}

static bool adaptive_cost_states_equal(
    const LzssAdaptiveAcCostState& left,
    const LzssAdaptiveAcCostState& right)
{
    const size_t repeat_distance_count =
        sizeof(left.repeat_distances) / sizeof(left.repeat_distances[0]);
    for (size_t i = 0; i < repeat_distance_count; ++i) {
        if (left.repeat_distances[i] != right.repeat_distances[i]) {
            return false;
        }
    }

    return left.literal_length_base_context ==
               right.literal_length_base_context &&
           left.literal_context == right.literal_context &&
           left.length_context == right.length_context &&
           left.distance_context == right.distance_context;
}

static bool adaptive_parent_state_matches(
    const AdaptiveOptimalParent& parent,
    const LzssAdaptiveAcCostState& cost_state,
    size_t literal_run_length)
{
    return parent.reachable &&
           parent.literal_run_length == literal_run_length &&
           adaptive_cost_states_equal(parent.cost_state, cost_state);
}

static AdaptiveOptimalParent& adaptive_parent_at(
    std::vector<AdaptiveOptimalParent> *parents,
    size_t pos,
    size_t slot)
{
    return (*parents)[pos * ADAPTIVE_OPTIMAL_BEAM_WIDTH + slot];
}

static const AdaptiveOptimalParent& adaptive_parent_at(
    const std::vector<AdaptiveOptimalParent>& parents,
    size_t pos,
    size_t slot)
{
    return parents[pos * ADAPTIVE_OPTIMAL_BEAM_WIDTH + slot];
}

static bool insert_adaptive_parent(
    std::vector<AdaptiveOptimalParent> *parents,
    size_t position_count,
    size_t pos,
    double cost,
    size_t prev_pos,
    size_t prev_slot,
    const LzssMatch& match,
    const LzssAdaptiveAcCostState& cost_state,
    size_t literal_run_length,
    bool is_match)
{
    if (parents == nullptr ||
        pos >= position_count ||
        prev_slot >= ADAPTIVE_OPTIMAL_BEAM_WIDTH ||
        !std::isfinite(cost)) {
        return false;
    }

    for (size_t slot = 0; slot < ADAPTIVE_OPTIMAL_BEAM_WIDTH; ++slot) {
        AdaptiveOptimalParent& parent =
            adaptive_parent_at(parents, pos, slot);
        if (!adaptive_parent_state_matches(
                parent,
                cost_state,
                literal_run_length)) {
            continue;
        }

        if (cost < parent.cost) {
            parent.cost = cost;
            parent.prev_pos = prev_pos;
            parent.prev_slot = prev_slot;
            parent.match = match;
            parent.cost_state = cost_state;
            parent.literal_run_length = literal_run_length;
            parent.is_match = is_match;
        }

        return true;
    }

    size_t target_slot = ADAPTIVE_OPTIMAL_BEAM_WIDTH;
    double worst_cost = -1.0;
    for (size_t slot = 0; slot < ADAPTIVE_OPTIMAL_BEAM_WIDTH; ++slot) {
        AdaptiveOptimalParent& parent =
            adaptive_parent_at(parents, pos, slot);
        if (!parent.reachable) {
            target_slot = slot;
            break;
        }

        if (target_slot == ADAPTIVE_OPTIMAL_BEAM_WIDTH ||
            parent.cost > worst_cost) {
            target_slot = slot;
            worst_cost = parent.cost;
        }
    }

    AdaptiveOptimalParent& target =
        adaptive_parent_at(parents, pos, target_slot);
    if (target.reachable && cost >= target.cost) {
        return true;
    }

    target.cost = cost;
    target.prev_pos = prev_pos;
    target.prev_slot = prev_slot;
    target.match = match;
    target.cost_state = cost_state;
    target.literal_run_length = literal_run_length;
    target.is_match = is_match;
    target.reachable = true;
    return true;
}

static size_t best_adaptive_parent_slot(
    const std::vector<AdaptiveOptimalParent>& parents,
    size_t pos)
{
    size_t best_slot = ADAPTIVE_OPTIMAL_BEAM_WIDTH;
    double best_cost = std::numeric_limits<double>::infinity();

    for (size_t slot = 0; slot < ADAPTIVE_OPTIMAL_BEAM_WIDTH; ++slot) {
        const AdaptiveOptimalParent& parent =
            adaptive_parent_at(parents, pos, slot);
        if (parent.reachable && parent.cost < best_cost) {
            best_cost = parent.cost;
            best_slot = slot;
        }
    }

    return best_slot;
}

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

bool lzss_encode_optimal_adaptive_ac(
    const uint8_t *input,
    size_t input_size,
    const LzssConfig *config,
    const LzssAdaptiveAcCostModel *cost_model,
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

    if (cost_model->literal_length_symbol_costs.empty() ||
        cost_model->literal_costs.empty() ||
        cost_model->length_symbol_costs.empty() ||
        cost_model->distance_symbol_costs.empty()) {
        return false;
    }

    LzssMatchFinder *match_finder =
        match_finder_create(config->window_size, config->hash_mode);
    if (match_finder == nullptr) {
        return false;
    }

    const size_t position_count = input_size + 1;
    std::vector<AdaptiveOptimalParent> parents(
        position_count * ADAPTIVE_OPTIMAL_BEAM_WIDTH
    );
    std::vector<LzssMatch> base_matches;
    std::vector<LzssMatch> matches;

    LzssAdaptiveAcCostState initial_state{};
    lzss_adaptive_ac_cost_state_init(&initial_state);

    double empty_literal_run_cost = 0.0;
    if (!lzss_adaptive_ac_literal_length_cost(
            cost_model,
            initial_state.literal_length_base_context,
            0,
            &empty_literal_run_cost)) {
        match_finder_destroy(match_finder);
        return false;
    }

    if (!insert_adaptive_parent(
            &parents,
            position_count,
            0,
            empty_literal_run_cost,
            0,
            0,
            LzssMatch{},
            initial_state,
            0,
            false)) {
        match_finder_destroy(match_finder);
        return false;
    }

    for (size_t pos = 0; pos < input_size; ++pos) {
        bool has_reachable_parent = false;
        for (size_t slot = 0; slot < ADAPTIVE_OPTIMAL_BEAM_WIDTH; ++slot) {
            if (adaptive_parent_at(parents, pos, slot).reachable) {
                has_reachable_parent = true;
                break;
            }
        }

        if (has_reachable_parent &&
            !match_finder_get_matches(
                match_finder,
                input,
                pos,
                input_size,
                config->min_match_length,
                config->max_match_length,
                &base_matches)) {
            match_finder_destroy(match_finder);
            return false;
        }

        for (size_t slot = 0; slot < ADAPTIVE_OPTIMAL_BEAM_WIDTH; ++slot) {
            const AdaptiveOptimalParent current_parent =
                adaptive_parent_at(parents, pos, slot);
            if (!current_parent.reachable) {
                continue;
            }

            double literal_delta_cost = 0.0;
            LzssAdaptiveAcCostState literal_state{};
            if (lzss_adaptive_ac_literal_transition_cost(
                    cost_model,
                    &current_parent.cost_state,
                    current_parent.literal_run_length,
                    input[pos],
                    &literal_delta_cost,
                    &literal_state)) {
                if (!insert_adaptive_parent(
                        &parents,
                        position_count,
                        pos + 1,
                        current_parent.cost + literal_delta_cost,
                        pos,
                        slot,
                        LzssMatch{},
                        literal_state,
                        current_parent.literal_run_length + 1,
                        false)) {
                    match_finder_destroy(match_finder);
                    return false;
                }
            }

            matches = base_matches;
            if (!append_repeat_distance_candidates(
                    input,
                    input_size,
                    pos,
                    config,
                    &current_parent.cost_state,
                    &matches)) {
                match_finder_destroy(match_finder);
                return false;
            }

            for (const LzssMatch& match : matches) {
                const size_t next_pos = pos + match.length;
                if (next_pos > input_size) {
                    continue;
                }

                double match_delta_cost = 0.0;
                LzssAdaptiveAcCostState match_state{};
                if (!lzss_adaptive_ac_match_transition_cost(
                        cost_model,
                        &current_parent.cost_state,
                        current_parent.literal_run_length,
                        match.length,
                        match.distance,
                        &match_delta_cost,
                        &match_state)) {
                    continue;
                }

                if (!insert_adaptive_parent(
                        &parents,
                        position_count,
                        next_pos,
                        current_parent.cost + match_delta_cost,
                        pos,
                        slot,
                        match,
                        match_state,
                        0,
                        true)) {
                    match_finder_destroy(match_finder);
                    return false;
                }
            }
        }

        match_finder_insert_position(match_finder, input, pos, input_size);
    }

    match_finder_destroy(match_finder);

    size_t slot = best_adaptive_parent_slot(parents, input_size);
    if (slot >= ADAPTIVE_OPTIMAL_BEAM_WIDTH) {
        return false;
    }

    std::vector<OptimalAction> actions;
    for (size_t pos = input_size; pos > 0;) {
        const AdaptiveOptimalParent& parent =
            adaptive_parent_at(parents, pos, slot);
        if (!parent.reachable || parent.prev_pos >= pos) {
            return false;
        }

        OptimalAction action{};
        action.is_match = parent.is_match;
        action.match = parent.match;
        actions.push_back(action);

        slot = parent.prev_slot;
        pos = parent.prev_pos;
    }

    std::reverse(actions.begin(), actions.end());
    return build_sequence_stream_from_actions(input, actions, out_stream);
}

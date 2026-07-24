#include "parser.h"
#include "mf.h"

#include <limits>

static bool fits_u32(size_t value)
{
    return value <= std::numeric_limits<uint32_t>::max();
}

static bool emit_sequence(LzssSequenceStream* out_stream,
                          const LzssMatch* match,
                          const uint8_t* literals_ptr,
                          size_t lit_length)
{
    if (!fits_u32(lit_length)) {
        return false;
    }

    LzssSequence seq{};
    if (match != nullptr) {
        seq.match_distance = match->distance;
        seq.match_length = match->length;
    }
    seq.lit_length = static_cast<uint32_t>(lit_length);
    seq.literals_ptr = lit_length > 0 ? literals_ptr : nullptr;

    sequence_stream_push(out_stream, seq);
    return true;
}

static bool find_best_match(LzssMatchFinder* mf,
                            const uint8_t* input,
                            size_t pos,
                            size_t input_size,
                            const LzssConfig* config,
                            LzssMatch* out_match)
{
    return match_finder_get_best(mf, input, pos, input_size,
                                 config->min_match_length,
                                 config->max_match_length,
                                 out_match);
}

static bool should_emit_literal_for_lazy_match(LzssMatchFinder* mf,
                                               const uint8_t* input,
                                               size_t pos,
                                               size_t input_size,
                                               const LzssConfig* config,
                                               const LzssMatch& current_match)
{
    if (config->parse_mode != LZSS_PARSE_LAZY || pos + 1 >= input_size) {
        return false;
    }

    LzssMatch next_match;
    const bool next_found =
        find_best_match(mf, input, pos + 1, input_size, config, &next_match);

    return next_found && next_match.length > current_match.length;
}

bool lzss_encode(const uint8_t* input, size_t input_size,
                 const LzssConfig* config, LzssSequenceStream* out_stream) {

    if ((input_size > 0 && !input) || !config || !out_stream) return false;

    LzssMatchFinder* mf = match_finder_create(config->window_size);
    if (!mf) return false;

    size_t pos = 0;
    size_t lit_start_pos = 0;
    bool has_pending_match = false;
    LzssMatch pending_match{};

    while (pos < input_size) {
        LzssMatch match;

        bool found = find_best_match(mf, input, pos, input_size,
                                     config, &match);

        if (found) {
            bool current_position_inserted = false;

            if (config->parse_mode == LZSS_PARSE_LAZY) {
                match_finder_insert_position(mf, input, pos, input_size);
                current_position_inserted = true;
            }

            if (should_emit_literal_for_lazy_match(mf, input, pos, input_size,
                                                   config, match)) {
                pos += 1;
                continue;
            }

            const size_t lit_length = pos - lit_start_pos;
            if (!emit_sequence(out_stream,
                               has_pending_match ? &pending_match : nullptr,
                               input + lit_start_pos,
                               lit_length)) {
                match_finder_destroy(mf);
                return false;
            }

            size_t insert_offset = current_position_inserted ? 1 : 0;
            for (size_t i = insert_offset; i < match.length; ++i) {
                match_finder_insert_position(mf, input, pos + i, input_size);
            }

            pos += match.length;
            pending_match = match;
            has_pending_match = true;
            lit_start_pos = pos;
        } else {
            match_finder_insert_position(mf, input, pos, input_size);
            pos += 1;
        }
    }

    const size_t final_lit_length = pos - lit_start_pos;
    if (has_pending_match || final_lit_length > 0) {
        if (!emit_sequence(out_stream,
                           has_pending_match ? &pending_match : nullptr,
                           input + lit_start_pos,
                           final_lit_length)) {
            match_finder_destroy(mf);
            return false;
        }
    }

    match_finder_destroy(mf);
    return true;
}


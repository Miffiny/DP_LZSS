#include "parser.h"
#include "mf.h"

static void emit_literal(LzssTokenStream* out_stream, uint8_t literal)
{
    LzssToken token;
    token.type = LZSS_TOKEN_LITERAL;
    token.literal = literal;
    token_stream_push(out_stream, token);
}

static void emit_match(LzssTokenStream* out_stream, const LzssMatch& match)
{
    LzssToken token;
    token.type = LZSS_TOKEN_MATCH;
    token.match.distance = match.distance;
    token.match.length = match.length;
    token_stream_push(out_stream, token);
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
                 const LzssConfig* config, LzssTokenStream* out_stream) {

    if ((input_size > 0 && !input) || !config || !out_stream) return false;

    LzssMatchFinder* mf = match_finder_create(config->window_size);
    if (!mf) return false;

    size_t pos = 0;

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
                emit_literal(out_stream, input[pos]);
                pos += 1;
                continue;
            }

            emit_match(out_stream, match);

            size_t insert_offset = current_position_inserted ? 1 : 0;
            for (size_t i = insert_offset; i < match.length; ++i) {
                match_finder_insert_position(mf, input, pos + i, input_size);
            }
            pos += match.length;
        } else {
            emit_literal(out_stream, input[pos]);

            match_finder_insert_position(mf, input, pos, input_size);
            pos += 1;
        }
    }

    LzssToken eof_token;
    eof_token.type = LZSS_TOKEN_EOF;
    token_stream_push(out_stream, eof_token);

    match_finder_destroy(mf);
    return true;
}

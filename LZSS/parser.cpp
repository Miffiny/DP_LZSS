#include "parser.h"
#include "mf.h"

bool lzss_encode(const uint8_t* input, size_t input_size,
                 const LzssConfig* config, LzssTokenStream* out_stream) {

    if ((input_size > 0 && !input) || !config || !out_stream) return false;

    LzssMatchFinder* mf = match_finder_create(config->window_size);
    if (!mf) return false;

    size_t pos = 0;

    while (pos < input_size) {
        LzssMatch match;

        bool found = match_finder_get_best(mf, input, pos, input_size,
                                           config->min_match_length,
                                           config->max_match_length,
                                           &match);

        if (found) {
            LzssToken token;
            token.type = LZSS_TOKEN_MATCH;
            token.match.distance = match.distance;
            token.match.length = match.length;
            token_stream_push(out_stream, token);

            for (size_t i = 0; i < match.length; ++i) {
                match_finder_insert_position(mf, input, pos + i);
            }
            pos += match.length;
        } else {
            LzssToken token;
            token.type = LZSS_TOKEN_LITERAL;
            token.literal = input[pos];
            token_stream_push(out_stream, token);

            match_finder_insert_position(mf, input, pos);
            pos += 1;
        }
    }

    LzssToken eof_token;
    eof_token.type = LZSS_TOKEN_EOF;
    token_stream_push(out_stream, eof_token);

    match_finder_destroy(mf);
    return true;
}

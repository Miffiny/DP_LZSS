#include "arithmetic.h"

#include <cstdlib>
#include <cstring>
#include <limits>

static bool is_valid_config(const LzssConfig *config)
{
    if (config == nullptr) {
        return false;
    }

    if (config->window_size == 0 ||
        config->window_size > UINT32_MAX) {
        return false;
    }

    if (config->min_match_length == 0 ||
        config->min_match_length > UINT32_MAX) {
        return false;
    }

    if (config->max_match_length < config->min_match_length ||
        config->max_match_length > UINT32_MAX) {
        return false;
    }

    return true;
}

static size_t bits_needed(size_t max_value)
{
    size_t bits = 0;

    while (max_value != 0) {
        ++bits;
        max_value >>= 1;
    }

    return bits;
}

static bool token_is_valid(
    const LzssArithmeticCodec *codec,
    const LzssToken *token)
{
    if (codec == nullptr || token == nullptr) {
        return false;
    }

    switch (token->type) {
    case LZSS_TOKEN_LITERAL:
    case LZSS_TOKEN_EOF:
        return true;

    case LZSS_TOKEN_MATCH:
        return token->match.distance >= 1 &&
               token->match.distance <= codec->config.window_size &&
               token->match.length >= codec->config.min_match_length &&
               token->match.length <= codec->config.max_match_length;

    default:
        return false;
    }
}

static void encode_distance(
    LzssArithmeticCodec *codec,
    struct ac *ac,
    struct bio *bio,
    uint32_t distance)
{
    const uint32_t value = distance - 1;

    for (size_t bit_index = 0;
         bit_index < codec->distance_bit_count;
         ++bit_index) {
        const size_t shift =
            codec->distance_bit_count - 1 - bit_index;

        const size_t bit =
            (value >> shift) & 1u;

        model *model =
            &codec->distance_bit_models[bit_index];

        ac_encode_symbol_model(ac, bio, bit, model);
        model_update(model, bit);
    }
}

static bool decode_distance(
    LzssArithmeticCodec *codec,
    struct ac *ac,
    struct bio *bio,
    uint32_t *out_distance)
{
    uint64_t value = 0;

    for (size_t bit_index = 0;
         bit_index < codec->distance_bit_count;
         ++bit_index) {
        struct model *model =
            &codec->distance_bit_models[bit_index];

        const size_t bit =
            ac_decode_symbol_model(ac, bio, model);

        if (bit > 1) {
            return false;
        }

        model_update(model, bit);

        value = (value << 1) | bit;
    }

    if (value >= codec->config.window_size) {
        return false;
    }

    *out_distance = (uint32_t)(value + 1);
    return true;
}

bool lzss_ac_codec_init(
    LzssArithmeticCodec *codec,
    const LzssConfig *config)
{
    if (codec == nullptr || !is_valid_config(config)) {
        return false;
    }

    std::memset(codec, 0, sizeof(*codec));
    codec->config = *config;

    codec->distance_bit_count =
        bits_needed(config->window_size - 1);

    const size_t length_symbol_count =
        config->max_match_length -
        config->min_match_length + 1;

    model_create(&codec->event_model, 3);
    model_create(&codec->literal_model, 256);
    model_create(&codec->length_model, length_symbol_count);

    if (codec->distance_bit_count > 0) {
        codec->distance_bit_models =
            static_cast<struct model *>(
                std::calloc(
                    codec->distance_bit_count,
                    sizeof(struct model)
                )
            );

        if (codec->distance_bit_models == nullptr) {
            lzss_ac_codec_destroy(codec);
            return false;
        }

        for (size_t i = 0;
             i < codec->distance_bit_count;
             ++i) {
            model_create(
                &codec->distance_bit_models[i],
                2
            );
        }
    }

    return true;
}

void lzss_ac_codec_destroy(
    LzssArithmeticCodec *codec)
{
    if (codec == nullptr) {
        return;
    }

    if (codec->event_model.table != nullptr) {
        model_destroy(&codec->event_model);
    }

    if (codec->literal_model.table != nullptr) {
        model_destroy(&codec->literal_model);
    }

    if (codec->length_model.table != nullptr) {
        model_destroy(&codec->length_model);
    }

    if (codec->distance_bit_models != nullptr) {
        for (size_t i = 0;
             i < codec->distance_bit_count;
             ++i) {
            if (codec->distance_bit_models[i].table != nullptr) {
                model_destroy(
                    &codec->distance_bit_models[i]
                );
            }
        }

        std::free(codec->distance_bit_models);
    }

    std::memset(codec, 0, sizeof(*codec));
}

bool lzss_ac_encode_stream(
    LzssArithmeticCodec *codec,
    struct ac *ac,
    struct bio *bio,
    const LzssTokenStream *stream)
{
    if (codec == nullptr ||
        ac == nullptr ||
        bio == nullptr ||
        stream == nullptr ||
        (stream->count > 0 && stream->tokens == nullptr)) {
        return false;
    }

    ac_init(ac);

    bool eof_seen = false;

    for (size_t i = 0; i < stream->count; ++i) {
        const LzssToken *token = &stream->tokens[i];

        if (!token_is_valid(codec, token)) {
            return false;
        }

        if (eof_seen) {
            return false;
        }

        if (token->type == LZSS_TOKEN_EOF &&
            i + 1 != stream->count) {
            return false;
        }

        ac_encode_symbol_model(
            ac,
            bio,
            token->type,
            &codec->event_model
        );

        model_update(
            &codec->event_model,
            token->type
        );

        switch (token->type) {
        case LZSS_TOKEN_LITERAL:
            ac_encode_symbol_model(
                ac,
                bio,
                token->literal,
                &codec->literal_model
            );

            model_update(
                &codec->literal_model,
                token->literal
            );
            break;

        case LZSS_TOKEN_MATCH: {
            const size_t length_symbol =
                token->match.length -
                codec->config.min_match_length;

            ac_encode_symbol_model(
                ac,
                bio,
                length_symbol,
                &codec->length_model
            );

            model_update(
                &codec->length_model,
                length_symbol
            );

            encode_distance(
                codec,
                ac,
                bio,
                token->match.distance
            );
            break;
        }

        case LZSS_TOKEN_EOF:
            eof_seen = true;
            break;

        default:
            return false;
        }
    }

    if (!eof_seen) {
        return false;
    }

    ac_encode_flush(ac, bio);
    return true;
}

bool lzss_ac_decode_stream(
    LzssArithmeticCodec *codec,
    struct ac *ac,
    struct bio *bio,
    LzssTokenStream *out_stream)
{
    if (codec == nullptr ||
        ac == nullptr ||
        bio == nullptr ||
        out_stream == nullptr) {
        return false;
    }

    ac_init(ac);
    ac_decode_init(ac, bio);

    for (;;) {
        const size_t event =
            ac_decode_symbol_model(
                ac,
                bio,
                &codec->event_model
            );

        if (event > LZSS_TOKEN_EOF) {
            return false;
        }

        model_update(
            &codec->event_model,
            event
        );

        LzssToken token{};
        token.type = (LzssTokenType)event;

        switch (token.type) {
        case LZSS_TOKEN_LITERAL:
            token.literal = (uint8_t)
                ac_decode_symbol_model(
                    ac,
                    bio,
                    &codec->literal_model
                );

            model_update(
                &codec->literal_model,
                token.literal
            );

            token_stream_push(out_stream, token);
            break;

        case LZSS_TOKEN_MATCH: {
            const size_t length_symbol =
                ac_decode_symbol_model(
                    ac,
                    bio,
                    &codec->length_model
                );

            if (length_symbol >= codec->length_model.count) {
                return false;
            }

            model_update(
                &codec->length_model,
                length_symbol
            );

            token.match.length =
                (uint32_t)(
                    codec->config.min_match_length +
                    length_symbol
                );

            if (!decode_distance(
                    codec,
                    ac,
                    bio,
                    &token.match.distance)) {
                return false;
            }

            token_stream_push(out_stream, token);
            break;
        }

        case LZSS_TOKEN_EOF:
            token_stream_push(out_stream, token);
            return true;

        default:
            return false;
        }
    }
}

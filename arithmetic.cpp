#include "arithmetic.h"

#include <cstdlib>
#include <cstring>
#include <limits>

struct DeflateClass {
    uint32_t base;
    uint32_t size;
    uint8_t extra_bits;
};

static const DeflateClass LENGTH_CLASSES[] = {
    {3, 1, 0},    {4, 1, 0},    {5, 1, 0},    {6, 1, 0},
    {7, 1, 0},    {8, 1, 0},    {9, 1, 0},    {10, 1, 0},
    {11, 2, 1},   {13, 2, 1},   {15, 2, 1},   {17, 2, 1},
    {19, 4, 2},   {23, 4, 2},   {27, 4, 2},   {31, 4, 2},
    {35, 8, 3},   {43, 8, 3},   {51, 8, 3},   {59, 8, 3},
    {67, 16, 4},  {83, 16, 4},  {99, 16, 4},  {115, 16, 4},
    {131, 32, 5}, {163, 32, 5}, {195, 32, 5}, {227, 31, 5},
    {258, 1, 0},
};

static const DeflateClass DISTANCE_CLASSES[] = {
    {1, 1, 0},       {2, 1, 0},       {3, 1, 0},
    {4, 1, 0},       {5, 2, 1},       {7, 2, 1},
    {9, 4, 2},       {13, 4, 2},      {17, 8, 3},
    {25, 8, 3},      {33, 16, 4},     {49, 16, 4},
    {65, 32, 5},     {97, 32, 5},     {129, 64, 6},
    {193, 64, 6},    {257, 128, 7},   {385, 128, 7},
    {513, 256, 8},   {769, 256, 8},   {1025, 512, 9},
    {1537, 512, 9},  {2049, 1024, 10}, {3073, 1024, 10},
    {4097, 2048, 11}, {6145, 2048, 11},
    {8193, 4096, 12}, {12289, 4096, 12},
    {16385, 8192, 13}, {24577, 8192, 13},
    {32769, 16384, 14}, {49153, 16384, 14},
};

template <typename T, size_t N>
static constexpr size_t array_count(const T (&)[N])
{
    return N;
}

static uint32_t class_last_value(const DeflateClass *cls)
{
    return cls->base + cls->size - 1;
}

static bool class_intersects_range(
    const DeflateClass *cls,
    uint32_t min_value,
    uint32_t max_value)
{
    return cls->base <= max_value &&
           class_last_value(cls) >= min_value;
}

static size_t active_class_count(
    const DeflateClass *classes,
    size_t class_count,
    uint32_t min_value,
    uint32_t max_value)
{
    size_t active_count = 0;

    for (size_t i = 0; i < class_count; ++i) {
        if (class_intersects_range(&classes[i], min_value, max_value)) {
            ++active_count;
        }
    }

    return active_count;
}

static size_t max_extra_bit_count(
    const DeflateClass *classes,
    size_t class_count,
    uint32_t min_value,
    uint32_t max_value)
{
    size_t max_bits = 0;

    for (size_t i = 0; i < class_count; ++i) {
        if (class_intersects_range(&classes[i], min_value, max_value) &&
            classes[i].extra_bits > max_bits) {
            max_bits = classes[i].extra_bits;
        }
    }

    return max_bits;
}

static bool find_class_for_value(
    const DeflateClass *classes,
    size_t class_count,
    uint32_t min_value,
    uint32_t max_value,
    uint32_t value,
    size_t *out_symbol,
    const DeflateClass **out_class)
{
    size_t symbol = 0;

    for (size_t i = 0; i < class_count; ++i) {
        const DeflateClass *cls = &classes[i];

        if (!class_intersects_range(cls, min_value, max_value)) {
            continue;
        }

        if (value >= cls->base && value <= class_last_value(cls)) {
            *out_symbol = symbol;
            *out_class = cls;
            return true;
        }

        ++symbol;
    }

    return false;
}

static bool find_class_by_symbol(
    const DeflateClass *classes,
    size_t class_count,
    uint32_t min_value,
    uint32_t max_value,
    size_t target_symbol,
    const DeflateClass **out_class)
{
    size_t symbol = 0;

    for (size_t i = 0; i < class_count; ++i) {
        const DeflateClass *cls = &classes[i];

        if (!class_intersects_range(cls, min_value, max_value)) {
            continue;
        }

        if (symbol == target_symbol) {
            *out_class = cls;
            return true;
        }

        ++symbol;
    }

    return false;
}

static bool create_bit_models(struct model **models, size_t count)
{
    *models = nullptr;

    if (count == 0) {
        return true;
    }

    *models = static_cast<struct model *>(
        std::calloc(count, sizeof(struct model))
    );

    if (*models == nullptr) {
        return false;
    }

    for (size_t i = 0; i < count; ++i) {
        model_create(&(*models)[i], 2);
    }

    return true;
}

static void destroy_bit_models(struct model *models, size_t count)
{
    if (models == nullptr) {
        return;
    }

    for (size_t i = 0; i < count; ++i) {
        if (models[i].table != nullptr) {
            model_destroy(&models[i]);
        }
    }

    std::free(models);
}

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

    if (config->min_match_length < LENGTH_CLASSES[0].base ||
        config->max_match_length >
            class_last_value(&LENGTH_CLASSES[array_count(LENGTH_CLASSES) - 1])) {
        return false;
    }

    if (config->window_size >
        class_last_value(&DISTANCE_CLASSES[array_count(DISTANCE_CLASSES) - 1])) {
        return false;
    }

    return true;
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

static void encode_extra_bits(
    struct ac *ac,
    struct bio *bio,
    struct model *bit_models,
    size_t bit_count,
    uint32_t value)
{
    for (size_t bit_index = 0;
         bit_index < bit_count;
         ++bit_index) {
        const size_t shift =
            bit_count - 1 - bit_index;

        const size_t bit =
            (value >> shift) & 1u;

        model *model =
            &bit_models[bit_index];

        ac_encode_symbol_model(ac, bio, bit, model);
        model_update(model, bit);
    }
}

static bool decode_extra_bits(
    struct ac *ac,
    struct bio *bio,
    struct model *bit_models,
    size_t bit_count,
    uint32_t *out_value)
{
    uint32_t value = 0;

    for (size_t bit_index = 0;
         bit_index < bit_count;
         ++bit_index) {
        struct model *model =
            &bit_models[bit_index];

        const size_t bit =
            ac_decode_symbol_model(ac, bio, model);

        if (bit > 1) {
            return false;
        }

        model_update(model, bit);

        value = (value << 1) | static_cast<uint32_t>(bit);
    }

    *out_value = value;
    return true;
}

static bool encode_length(
    LzssArithmeticCodec *codec,
    struct ac *ac,
    struct bio *bio,
    uint32_t length)
{
    size_t length_symbol = 0;
    const DeflateClass *length_class = nullptr;

    if (!find_class_for_value(
            LENGTH_CLASSES,
            array_count(LENGTH_CLASSES),
            static_cast<uint32_t>(codec->config.min_match_length),
            static_cast<uint32_t>(codec->config.max_match_length),
            length,
            &length_symbol,
            &length_class)) {
        return false;
    }

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

    encode_extra_bits(
        ac,
        bio,
        codec->length_extra_bit_models,
        length_class->extra_bits,
        length - length_class->base
    );

    return true;
}

static bool decode_length(
    LzssArithmeticCodec *codec,
    struct ac *ac,
    struct bio *bio,
    uint32_t *out_length)
{
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

    const DeflateClass *length_class = nullptr;

    if (!find_class_by_symbol(
            LENGTH_CLASSES,
            array_count(LENGTH_CLASSES),
            static_cast<uint32_t>(codec->config.min_match_length),
            static_cast<uint32_t>(codec->config.max_match_length),
            length_symbol,
            &length_class)) {
        return false;
    }

    uint32_t extra_value = 0;

    if (!decode_extra_bits(
            ac,
            bio,
            codec->length_extra_bit_models,
            length_class->extra_bits,
            &extra_value)) {
        return false;
    }

    if (extra_value >= length_class->size) {
        return false;
    }

    const uint32_t length =
        length_class->base + extra_value;

    if (length < codec->config.min_match_length ||
        length > codec->config.max_match_length) {
        return false;
    }

    *out_length = length;
    return true;
}

static bool encode_distance(
    LzssArithmeticCodec *codec,
    struct ac *ac,
    struct bio *bio,
    uint32_t distance)
{
    size_t distance_symbol = 0;
    const DeflateClass *distance_class = nullptr;

    if (!find_class_for_value(
            DISTANCE_CLASSES,
            array_count(DISTANCE_CLASSES),
            1,
            static_cast<uint32_t>(codec->config.window_size),
            distance,
            &distance_symbol,
            &distance_class)) {
        return false;
    }

    ac_encode_symbol_model(
        ac,
        bio,
        distance_symbol,
        &codec->distance_model
    );

    model_update(
        &codec->distance_model,
        distance_symbol
    );

    encode_extra_bits(
        ac,
        bio,
        codec->distance_extra_bit_models,
        distance_class->extra_bits,
        distance - distance_class->base
    );

    return true;
}

static bool decode_distance(
    LzssArithmeticCodec *codec,
    struct ac *ac,
    struct bio *bio,
    uint32_t *out_distance)
{
    const size_t distance_symbol =
        ac_decode_symbol_model(
            ac,
            bio,
            &codec->distance_model
        );

    if (distance_symbol >= codec->distance_model.count) {
        return false;
    }

    model_update(
        &codec->distance_model,
        distance_symbol
    );

    const DeflateClass *distance_class = nullptr;

    if (!find_class_by_symbol(
            DISTANCE_CLASSES,
            array_count(DISTANCE_CLASSES),
            1,
            static_cast<uint32_t>(codec->config.window_size),
            distance_symbol,
            &distance_class)) {
        return false;
    }

    uint32_t extra_value = 0;

    if (!decode_extra_bits(
            ac,
            bio,
            codec->distance_extra_bit_models,
            distance_class->extra_bits,
            &extra_value)) {
        return false;
    }

    if (extra_value >= distance_class->size) {
        return false;
    }

    const uint32_t distance =
        distance_class->base + extra_value;

    if (distance == 0 || distance > codec->config.window_size) {
        return false;
    }

    *out_distance = distance;
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

    const size_t length_class_count =
        active_class_count(
            LENGTH_CLASSES,
            array_count(LENGTH_CLASSES),
            static_cast<uint32_t>(config->min_match_length),
            static_cast<uint32_t>(config->max_match_length)
        );

    const size_t distance_class_count =
        active_class_count(
            DISTANCE_CLASSES,
            array_count(DISTANCE_CLASSES),
            1,
            static_cast<uint32_t>(config->window_size)
        );

    codec->length_extra_bit_count =
        max_extra_bit_count(
            LENGTH_CLASSES,
            array_count(LENGTH_CLASSES),
            static_cast<uint32_t>(config->min_match_length),
            static_cast<uint32_t>(config->max_match_length)
        );

    codec->distance_extra_bit_count =
        max_extra_bit_count(
            DISTANCE_CLASSES,
            array_count(DISTANCE_CLASSES),
            1,
            static_cast<uint32_t>(config->window_size)
        );

    model_create(&codec->event_model, 3);
    model_create(&codec->literal_model, 256);
    model_create(&codec->length_model, length_class_count);
    model_create(&codec->distance_model, distance_class_count);

    if (!create_bit_models(
            &codec->length_extra_bit_models,
            codec->length_extra_bit_count) ||
        !create_bit_models(
            &codec->distance_extra_bit_models,
            codec->distance_extra_bit_count)) {
        lzss_ac_codec_destroy(codec);
        return false;
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

    if (codec->distance_model.table != nullptr) {
        model_destroy(&codec->distance_model);
    }

    destroy_bit_models(
        codec->length_extra_bit_models,
        codec->length_extra_bit_count
    );
    destroy_bit_models(
        codec->distance_extra_bit_models,
        codec->distance_extra_bit_count
    );

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
            if (!encode_length(
                    codec,
                    ac,
                    bio,
                    token->match.length)) {
                return false;
            }

            if (!encode_distance(
                    codec,
                    ac,
                    bio,
                    token->match.distance)) {
                return false;
            }
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
            if (!decode_length(
                    codec,
                    ac,
                    bio,
                    &token.match.length)) {
                return false;
            }

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

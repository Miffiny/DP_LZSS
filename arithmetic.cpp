#include "arithmetic.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

static constexpr size_t STATIC_MODEL_MAX_TOTAL = 1u << 12;

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

static bool write_u32(struct bio *bio, uint32_t value)
{
    ac_encode_bypass_bits(bio, value, 32);
    return true;
}

static uint32_t read_u32(struct bio *bio)
{
    return ac_decode_bypass_bits(bio, 32);
}

static bool checked_size_to_u32(size_t value, uint32_t *out_value)
{
    if (value > UINT32_MAX) {
        return false;
    }

    *out_value = static_cast<uint32_t>(value);
    return true;
}

static bool model_set_frequencies(
    struct model *model,
    const std::vector<uint32_t>& frequencies)
{
    if (model == nullptr ||
        model->table == nullptr ||
        model->count != frequencies.size()) {
        return false;
    }

    std::vector<uint32_t> scaled_frequencies = frequencies;
    size_t total = 0;

    for (uint32_t frequency : scaled_frequencies) {
        if (total > std::numeric_limits<size_t>::max() -
                        static_cast<size_t>(frequency)) {
            return false;
        }

        total += frequency;
    }

    if (total > STATIC_MODEL_MAX_TOTAL) {
        size_t positive_count = 0;
        for (uint32_t frequency : scaled_frequencies) {
            if (frequency != 0) {
                ++positive_count;
            }
        }

        if (positive_count > STATIC_MODEL_MAX_TOTAL) {
            return false;
        }

        std::vector<uint64_t> remainders(model->count, 0);
        size_t scaled_total = 0;

        for (size_t i = 0; i < model->count; ++i) {
            if (scaled_frequencies[i] == 0) {
                continue;
            }

            const uint64_t product =
                static_cast<uint64_t>(scaled_frequencies[i]) *
                static_cast<uint64_t>(STATIC_MODEL_MAX_TOTAL);
            uint32_t scaled =
                static_cast<uint32_t>(product / total);

            if (scaled == 0) {
                scaled = 1;
            }

            scaled_frequencies[i] = scaled;
            remainders[i] = product % total;
            scaled_total += scaled;
        }

        while (scaled_total > STATIC_MODEL_MAX_TOTAL) {
            size_t reduce_index = model->count;

            for (size_t i = 0; i < model->count; ++i) {
                if (scaled_frequencies[i] <= 1) {
                    continue;
                }

                if (reduce_index == model->count ||
                    scaled_frequencies[i] > scaled_frequencies[reduce_index]) {
                    reduce_index = i;
                }
            }

            if (reduce_index == model->count) {
                return false;
            }

            --scaled_frequencies[reduce_index];
            --scaled_total;
        }

        while (scaled_total < STATIC_MODEL_MAX_TOTAL) {
            size_t increase_index = model->count;

            for (size_t i = 0; i < model->count; ++i) {
                if (scaled_frequencies[i] == 0) {
                    continue;
                }

                if (increase_index == model->count ||
                    remainders[i] > remainders[increase_index]) {
                    increase_index = i;
                }
            }

            if (increase_index == model->count) {
                return false;
            }

            ++scaled_frequencies[increase_index];
            ++scaled_total;
            remainders[increase_index] = 0;
        }

        total = scaled_total;
    }

    for (size_t i = 0; i < model->count; ++i) {
        model->table[i].symb = i;
        model->table[i].freq = scaled_frequencies[i];
    }

    count_cum_freqs(model->table, model->count);
    model->total = total;
    return model_build_decode_lut(model, STATIC_MODEL_MAX_TOTAL) != 0;
}

static bool write_model_frequencies(struct bio *bio, const struct model *model)
{
    if (bio == nullptr || model == nullptr || model->table == nullptr) {
        return false;
    }

    for (size_t i = 0; i < model->count; ++i) {
        uint32_t freq = 0;

        if (!checked_size_to_u32(model->table[i].freq, &freq) ||
            !write_u32(bio, freq)) {
            return false;
        }
    }

    return true;
}

static bool read_model_frequencies(struct bio *bio, struct model *model)
{
    if (bio == nullptr || model == nullptr || model->table == nullptr) {
        return false;
    }

    std::vector<uint32_t> frequencies(model->count);

    for (size_t i = 0; i < model->count; ++i) {
        frequencies[i] = read_u32(bio);
    }

    return model_set_frequencies(model, frequencies);
}

static bool write_static_model_header(
    struct bio *bio,
    const LzssArithmeticCodec *codec)
{
    return write_model_frequencies(bio, &codec->event_model) &&
           write_model_frequencies(bio, &codec->literal_model) &&
           write_model_frequencies(bio, &codec->length_model) &&
           write_model_frequencies(bio, &codec->distance_model);
}

static bool read_static_model_header(
    struct bio *bio,
    LzssArithmeticCodec *codec)
{
    if (!read_model_frequencies(bio, &codec->event_model) ||
        !read_model_frequencies(bio, &codec->literal_model) ||
        !read_model_frequencies(bio, &codec->length_model) ||
        !read_model_frequencies(bio, &codec->distance_model)) {
        return false;
    }

    return codec->event_model.total > 0;
}

static bool collect_static_model_stats(
    LzssArithmeticCodec *codec,
    const LzssTokenStream *stream)
{
    if (codec == nullptr || stream == nullptr) {
        return false;
    }

    std::vector<uint32_t> event_frequencies(codec->event_model.count, 0);
    std::vector<uint32_t> literal_frequencies(codec->literal_model.count, 0);
    std::vector<uint32_t> length_frequencies(codec->length_model.count, 0);
    std::vector<uint32_t> distance_frequencies(codec->distance_model.count, 0);

    bool eof_seen = false;

    const size_t token_count = stream->tokens.size();

    for (size_t i = 0; i < token_count; ++i) {
        const LzssToken *token = &stream->tokens[i];

        if (!token_is_valid(codec, token) || eof_seen) {
            return false;
        }

        if (token->type == LZSS_TOKEN_EOF &&
            i + 1 != token_count) {
            return false;
        }

        event_frequencies[token->type]++;

        switch (token->type) {
        case LZSS_TOKEN_LITERAL:
            literal_frequencies[token->literal]++;
            break;

        case LZSS_TOKEN_MATCH: {
            size_t length_symbol = 0;
            const DeflateClass *length_class = nullptr;

            if (!find_class_for_value(
                    LENGTH_CLASSES,
                    array_count(LENGTH_CLASSES),
                    static_cast<uint32_t>(codec->config.min_match_length),
                    static_cast<uint32_t>(codec->config.max_match_length),
                    token->match.length,
                    &length_symbol,
                    &length_class)) {
                return false;
            }

            size_t distance_symbol = 0;
            const DeflateClass *distance_class = nullptr;

            if (!find_class_for_value(
                    DISTANCE_CLASSES,
                    array_count(DISTANCE_CLASSES),
                    1,
                    static_cast<uint32_t>(codec->config.window_size),
                    token->match.distance,
                    &distance_symbol,
                    &distance_class)) {
                return false;
            }

            length_frequencies[length_symbol]++;
            distance_frequencies[distance_symbol]++;
            break;
        }

        case LZSS_TOKEN_EOF:
            eof_seen = true;
            break;

        default:
            return false;
        }
    }

    return eof_seen &&
           model_set_frequencies(&codec->event_model, event_frequencies) &&
           model_set_frequencies(&codec->literal_model, literal_frequencies) &&
           model_set_frequencies(&codec->length_model, length_frequencies) &&
           model_set_frequencies(&codec->distance_model, distance_frequencies);
}

static bool encode_length_static(
    LzssArithmeticCodec *codec,
    struct ac *ac,
    struct bio *ac_bio,
    struct bio *extra_bio,
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
        ac_bio,
        length_symbol,
        &codec->length_model
    );

    ac_encode_bypass_bits(
        extra_bio,
        length - length_class->base,
        length_class->extra_bits
    );

    return true;
}

static bool decode_length_static(
    LzssArithmeticCodec *codec,
    struct ac *ac,
    struct bio *ac_bio,
    struct bio *extra_bio,
    uint32_t *out_length)
{
    if (codec->length_model.total == 0) {
        return false;
    }

    const size_t length_symbol =
        ac_decode_symbol_model(
            ac,
            ac_bio,
            &codec->length_model
        );

    if (length_symbol >= codec->length_model.count) {
        return false;
    }

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

    const uint32_t extra_value =
        ac_decode_bypass_bits(extra_bio, length_class->extra_bits);

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

static bool encode_distance_static(
    LzssArithmeticCodec *codec,
    struct ac *ac,
    struct bio *ac_bio,
    struct bio *extra_bio,
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
        ac_bio,
        distance_symbol,
        &codec->distance_model
    );

    ac_encode_bypass_bits(
        extra_bio,
        distance - distance_class->base,
        distance_class->extra_bits
    );

    return true;
}

static bool decode_distance_static(
    LzssArithmeticCodec *codec,
    struct ac *ac,
    struct bio *ac_bio,
    struct bio *extra_bio,
    uint32_t *out_distance)
{
    if (codec->distance_model.total == 0) {
        return false;
    }

    const size_t distance_symbol =
        ac_decode_symbol_model(
            ac,
            ac_bio,
            &codec->distance_model
        );

    if (distance_symbol >= codec->distance_model.count) {
        return false;
    }

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

    const uint32_t extra_value =
        ac_decode_bypass_bits(extra_bio, distance_class->extra_bits);

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
        stream == nullptr) {
        return false;
    }

    if (!collect_static_model_stats(codec, stream) ||
        !write_static_model_header(bio, codec)) {
        return false;
    }

    const size_t token_count = stream->tokens.size();
    const size_t ac_word_capacity =
        std::max<size_t>(1024, token_count * 8 + 64);
    const size_t extra_word_capacity =
        std::max<size_t>(16, token_count + 64);

    std::vector<uint32_t> ac_words(ac_word_capacity, 0);
    std::vector<uint32_t> extra_words(extra_word_capacity, 0);

    struct bio ac_writer{};
    struct bio extra_writer{};

    bio_open(
        &ac_writer,
        ac_words.data(),
        ac_words.data() + ac_words.size(),
        BIO_MODE_WRITE
    );

    bio_open(
        &extra_writer,
        extra_words.data(),
        extra_words.data() + extra_words.size(),
        BIO_MODE_WRITE
    );

    ac_init(ac);

    bool eof_seen = false;

    for (size_t i = 0; i < token_count; ++i) {
        const LzssToken *token = &stream->tokens[i];

        if (!token_is_valid(codec, token)) {
            return false;
        }

        if (eof_seen) {
            return false;
        }

        if (token->type == LZSS_TOKEN_EOF &&
            i + 1 != token_count) {
            return false;
        }

        ac_encode_symbol_model(
            ac,
            &ac_writer,
            token->type,
            &codec->event_model
        );

        switch (token->type) {
        case LZSS_TOKEN_LITERAL:
            ac_encode_symbol_model(
                ac,
                &ac_writer,
                token->literal,
                &codec->literal_model
            );

            break;

        case LZSS_TOKEN_MATCH: {
            if (!encode_length_static(
                    codec,
                    ac,
                    &ac_writer,
                    &extra_writer,
                    token->match.length)) {
                return false;
            }

            if (!encode_distance_static(
                    codec,
                    ac,
                    &ac_writer,
                    &extra_writer,
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

    ac_encode_flush(ac, &ac_writer);
    bio_close(&ac_writer, BIO_MODE_WRITE);
    bio_close(&extra_writer, BIO_MODE_WRITE);

    const size_t ac_word_count =
        static_cast<size_t>(ac_writer.ptr - ac_words.data());
    const size_t extra_word_count =
        static_cast<size_t>(extra_writer.ptr - extra_words.data());

    uint32_t ac_word_count_u32 = 0;
    uint32_t extra_word_count_u32 = 0;

    if (!checked_size_to_u32(ac_word_count, &ac_word_count_u32) ||
        !checked_size_to_u32(extra_word_count, &extra_word_count_u32) ||
        !write_u32(bio, ac_word_count_u32) ||
        !write_u32(bio, extra_word_count_u32)) {
        return false;
    }

    for (size_t i = 0; i < ac_word_count; ++i) {
        write_u32(bio, ac_words[i]);
    }

    for (size_t i = 0; i < extra_word_count; ++i) {
        write_u32(bio, extra_words[i]);
    }

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

    if (!read_static_model_header(bio, codec)) {
        return false;
    }

    const uint32_t ac_word_count = read_u32(bio);
    const uint32_t extra_word_count = read_u32(bio);

    std::vector<uint32_t> ac_words(ac_word_count, 0);
    std::vector<uint32_t> extra_words(extra_word_count, 0);

    for (uint32_t i = 0; i < ac_word_count; ++i) {
        ac_words[i] = read_u32(bio);
    }

    for (uint32_t i = 0; i < extra_word_count; ++i) {
        extra_words[i] = read_u32(bio);
    }

    struct bio ac_reader{};
    struct bio extra_reader{};

    bio_open(
        &ac_reader,
        ac_words.data(),
        ac_words.data() + ac_words.size(),
        BIO_MODE_READ
    );

    bio_open(
        &extra_reader,
        extra_words.data(),
        extra_words.data() + extra_words.size(),
        BIO_MODE_READ
    );

    ac_init(ac);
    ac_decode_init(ac, &ac_reader);

    for (;;) {
        const size_t event =
            ac_decode_symbol_model(
                ac,
                &ac_reader,
                &codec->event_model
            );

        if (event > LZSS_TOKEN_EOF) {
            return false;
        }

        LzssToken token{};
        token.type = (LzssTokenType)event;

        switch (token.type) {
        case LZSS_TOKEN_LITERAL:
            if (codec->literal_model.total == 0) {
                return false;
            }

            token.literal = (uint8_t)
                ac_decode_symbol_model(
                    ac,
                    &ac_reader,
                    &codec->literal_model
                );

            token_stream_push(out_stream, token);
            break;

        case LZSS_TOKEN_MATCH: {
            if (!decode_length_static(
                    codec,
                    ac,
                    &ac_reader,
                    &extra_reader,
                    &token.match.length)) {
                return false;
            }

            if (!decode_distance_static(
                    codec,
                    ac,
                    &ac_reader,
                    &extra_reader,
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

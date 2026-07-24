#include "tans.h"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <vector>

struct tans_bit_chunk {
    uint32_t bits;
    uint8_t count;
};

int tans_model_init(struct tans_model *tm, const struct model *m)
{
    if (tm == NULL || m == NULL || m->total != TANS_L) {
        return 0;
    }

    tm->count = m->count;
    tm->symbol_cum = (uint16_t*)malloc(m->count * sizeof(uint16_t));
    tm->symbol_freqs = (uint16_t*)malloc(m->count * sizeof(uint16_t));

    if (!tm->symbol_cum || !tm->symbol_freqs) {
        tans_model_destroy(tm);
        return 0;
    }

    for (size_t s = 0; s < m->count; ++s) {
        if (m->table[s].symb >= m->count ||
            m->table[s].freq > TANS_L ||
            m->table[s].cum_freq > TANS_L ||
            m->table[s].cum_freq + m->table[s].freq > TANS_L) {
            tans_model_destroy(tm);
            return 0;
        }

        tm->symbol_cum[s] = m->table[s].cum_freq;
        tm->symbol_freqs[s] = m->table[s].freq;
    }

    for (size_t s = 0; s < m->count; ++s) {
        const uint32_t freq = tm->symbol_freqs[s];
        const uint32_t cum = tm->symbol_cum[s];

        if (freq == 0) {
            continue;
        }

        for (uint32_t slot = cum; slot < cum + freq; ++slot) {
            const uint32_t decoded_x = freq + slot - cum;
            uint32_t base_x = decoded_x;
            uint8_t bits = 0;

            while (base_x < TANS_L) {
                base_x <<= 1;
                ++bits;
            }

            tm->decode_table[slot].symbol = (uint16_t)s;
            tm->decode_table[slot].num_bits = bits;
            tm->decode_table[slot].new_x = (uint16_t)base_x;
        }
    }

    return 1;
}

void tans_model_destroy(struct tans_model *tm)
{
    if (tm) {
        free(tm->symbol_cum);
        free(tm->symbol_freqs);
        tm->symbol_cum = NULL;
        tm->symbol_freqs = NULL;
    }
}

void tans_encode_init(struct tans_state *ts)
{
    ts->x = TANS_L;
}

static struct tans_bit_chunk tans_encode_symbol_to_chunk(
    struct tans_state *ts,
    size_t symb,
    const struct tans_model *tm)
{
    uint32_t freq = tm->symbol_freqs[symb];
    uint32_t cum = tm->symbol_cum[symb];
    uint32_t max_x_for_symbol = (freq << 1);

    struct tans_bit_chunk chunk{};
    uint32_t temp_x = ts->x;

    while (temp_x >= max_x_for_symbol) {
        chunk.count++;
        temp_x >>= 1;
    }

    if (chunk.count > 0) {
        chunk.bits = ts->x & (((uint32_t)1 << chunk.count) - 1);
    }

    ts->x = TANS_L + cum + temp_x - freq;
    return chunk;
}

void tans_encode_flush(struct tans_state *ts, struct bio *bio)
{
    bio_write_bits(bio, ts->x - TANS_L, 12);
}

void tans_decode_init(struct tans_state *ts, struct bio *bio)
{
    ts->x = bio_read_bits(bio, 12) + TANS_L;
}

size_t tans_decode_symbol(struct tans_state *ts, struct bio *bio, const struct tans_model *tm)
{
    const struct tans_decode_entry *entry = &tm->decode_table[ts->x - TANS_L];

    uint32_t new_bits = bio_read_bits(bio, entry->num_bits);

    ts->x = entry->new_x + new_bits;

    return entry->symbol;
}

static constexpr size_t STATIC_MODEL_TOTAL = TANS_L;

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

static const DeflateClass LITERAL_LENGTH_CLASSES[] = {
    {0, 1, 0},             {1, 1, 0},
    {2, 2, 1},             {4, 4, 2},
    {8, 8, 3},             {16, 16, 4},
    {32, 32, 5},           {64, 64, 6},
    {128, 128, 7},         {256, 256, 8},
    {512, 512, 9},         {1024, 1024, 10},
    {2048, 2048, 11},      {4096, 4096, 12},
    {8192, 8192, 13},      {16384, 16384, 14},
    {32768, 32768, 15},    {65536, 65536, 16},
    {131072, 131072, 17},  {262144, 262144, 18},
    {524288, 524288, 19},  {1048576, 1048576, 20},
    {2097152, 2097152, 21}, {4194304, 4194304, 22},
    {8388608, 8388608, 23}, {16777216, 16777216, 24},
    {33554432, 33554432, 25}, {67108864, 67108864, 26},
    {134217728, 134217728, 27}, {268435456, 268435456, 28},
    {536870912, 536870912, 29}, {1073741824, 1073741824, 30},
    {2147483648u, 2147483648u, 31},
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

static uint64_t class_last_value(const DeflateClass *cls)
{
    return static_cast<uint64_t>(cls->base) +
           static_cast<uint64_t>(cls->size) - 1;
}

static bool class_intersects_range(
    const DeflateClass *cls,
    uint32_t min_value,
    uint32_t max_value)
{
    return static_cast<uint64_t>(cls->base) <= max_value &&
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

        if (value >= cls->base &&
            static_cast<uint64_t>(value) <= class_last_value(cls)) {
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
        static_cast<uint64_t>(config->max_match_length) >
            class_last_value(&LENGTH_CLASSES[array_count(LENGTH_CLASSES) - 1])) {
        return false;
    }

    if (static_cast<uint64_t>(config->window_size) >
        class_last_value(&DISTANCE_CLASSES[array_count(DISTANCE_CLASSES) - 1])) {
        return false;
    }

    return true;
}

static bool sequence_match_is_valid(
    const LzssTansCodec *codec,
    const LzssSequence *sequence)
{
    if (codec == nullptr || sequence == nullptr) {
        return false;
    }

    if (sequence->match_length == 0) {
        return sequence->match_distance == 0;
    }

    return sequence->match_distance >= 1 &&
           sequence->match_distance <= codec->config.window_size &&
           sequence->match_length >= codec->config.min_match_length &&
           sequence->match_length <= codec->config.max_match_length;
}

static bool sequence_layout_is_valid(
    const LzssTansCodec *codec,
    const LzssSequenceStream *stream)
{
    if (codec == nullptr || stream == nullptr) {
        return false;
    }

    for (size_t i = 0; i < stream->sequences.size(); ++i) {
        const LzssSequence& sequence = stream->sequences[i];

        if (!sequence_match_is_valid(codec, &sequence)) {
            return false;
        }

        if (sequence.lit_length > 0 && sequence.literals_ptr == nullptr) {
            return false;
        }

        if (i == 0) {
            if (sequence.match_length != 0 ||
                sequence.match_distance != 0) {
                return false;
            }
        } else if (sequence.match_length == 0) {
            return false;
        }
    }

    return true;
}

static bool checked_size_to_u32(size_t value, uint32_t *out_value)
{
    if (value > UINT32_MAX) {
        return false;
    }

    *out_value = static_cast<uint32_t>(value);
    return true;
}

static bool write_u32(struct bio *bio, uint32_t value)
{
    bio_write_bits(bio, value, 32);
    return true;
}

static uint32_t read_u32(struct bio *bio)
{
    return bio_read_bits(bio, 32);
}

static bool model_set_frequencies_tans(
    struct model *model,
    const std::vector<uint32_t>& frequencies)
{
    if (model == nullptr ||
        model->table == nullptr ||
        model->count != frequencies.size()) {
        return false;
    }

    size_t total = 0;
    size_t positive_count = 0;

    for (uint32_t frequency : frequencies) {
        if (total > std::numeric_limits<size_t>::max() -
                        static_cast<size_t>(frequency)) {
            return false;
        }

        total += frequency;

        if (frequency != 0) {
            ++positive_count;
        }
    }

    std::vector<uint32_t> scaled_frequencies(model->count, 0);

    if (total != 0) {
        if (positive_count > STATIC_MODEL_TOTAL) {
            return false;
        }

        std::vector<uint64_t> remainders(model->count, 0);
        size_t scaled_total = 0;

        for (size_t i = 0; i < model->count; ++i) {
            if (frequencies[i] == 0) {
                continue;
            }

            const uint64_t product =
                static_cast<uint64_t>(frequencies[i]) *
                static_cast<uint64_t>(STATIC_MODEL_TOTAL);
            uint32_t scaled =
                static_cast<uint32_t>(product / total);

            if (scaled == 0) {
                scaled = 1;
            }

            scaled_frequencies[i] = scaled;
            remainders[i] = product % total;
            scaled_total += scaled;
        }

        while (scaled_total > STATIC_MODEL_TOTAL) {
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

        while (scaled_total < STATIC_MODEL_TOTAL) {
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

    model_clear_decode_lut(model);
    count_cum_freqs(model->table, model->count);
    model->total = total;
    return true;
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

    return model_set_frequencies_tans(model, frequencies);
}

static bool write_static_model_header(
    struct bio *bio,
    const LzssTansCodec *codec)
{
    return write_model_frequencies(bio, &codec->literal_length_model) &&
           write_model_frequencies(bio, &codec->literal_model) &&
           write_model_frequencies(bio, &codec->length_model) &&
           write_model_frequencies(bio, &codec->distance_model);
}

static bool read_static_model_header(
    struct bio *bio,
    LzssTansCodec *codec)
{
    if (!read_model_frequencies(bio, &codec->literal_length_model) ||
        !read_model_frequencies(bio, &codec->literal_model) ||
        !read_model_frequencies(bio, &codec->length_model) ||
        !read_model_frequencies(bio, &codec->distance_model)) {
        return false;
    }

    return true;
}

static bool collect_static_model_stats(
    LzssTansCodec *codec,
    const LzssSequenceStream *stream)
{
    if (codec == nullptr || !sequence_layout_is_valid(codec, stream)) {
        return false;
    }

    std::vector<uint32_t> literal_length_frequencies(
        codec->literal_length_model.count,
        0
    );
    std::vector<uint32_t> literal_frequencies(codec->literal_model.count, 0);
    std::vector<uint32_t> length_frequencies(codec->length_model.count, 0);
    std::vector<uint32_t> distance_frequencies(codec->distance_model.count, 0);

    for (size_t i = 0; i < stream->sequences.size(); ++i) {
        const LzssSequence& sequence = stream->sequences[i];

        size_t literal_length_symbol = 0;
        const DeflateClass *literal_length_class = nullptr;

        if (!find_class_for_value(
                LITERAL_LENGTH_CLASSES,
                array_count(LITERAL_LENGTH_CLASSES),
                0,
                UINT32_MAX,
                sequence.lit_length,
                &literal_length_symbol,
                &literal_length_class)) {
            return false;
        }

        (void)literal_length_class;
        literal_length_frequencies[literal_length_symbol]++;

        for (uint32_t literal_index = 0;
             literal_index < sequence.lit_length;
             ++literal_index) {
            literal_frequencies[sequence.literals_ptr[literal_index]]++;
        }

        if (i == 0) {
            continue;
        }

        size_t length_symbol = 0;
        const DeflateClass *length_class = nullptr;

        if (!find_class_for_value(
                LENGTH_CLASSES,
                array_count(LENGTH_CLASSES),
                static_cast<uint32_t>(codec->config.min_match_length),
                static_cast<uint32_t>(codec->config.max_match_length),
                sequence.match_length,
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
                sequence.match_distance,
                &distance_symbol,
                &distance_class)) {
            return false;
        }

        (void)length_class;
        (void)distance_class;
        length_frequencies[length_symbol]++;
        distance_frequencies[distance_symbol]++;
    }

    return model_set_frequencies_tans(
               &codec->literal_length_model,
               literal_length_frequencies) &&
           model_set_frequencies_tans(&codec->literal_model, literal_frequencies) &&
           model_set_frequencies_tans(&codec->length_model, length_frequencies) &&
           model_set_frequencies_tans(&codec->distance_model, distance_frequencies);
}

static bool init_tans_model_if_used(
    struct tans_model *tans_model,
    const struct model *model,
    bool *ready)
{
    *ready = false;

    if (model->total == 0) {
        return true;
    }

    if (model->total != STATIC_MODEL_TOTAL ||
        !tans_model_init(tans_model, model)) {
        return false;
    }

    *ready = true;
    return true;
}

static bool init_all_tans_models(LzssTansCodec *codec)
{
    return init_tans_model_if_used(
               &codec->literal_length_tans_model,
               &codec->literal_length_model,
               &codec->literal_length_tans_ready) &&
           init_tans_model_if_used(
               &codec->literal_tans_model,
               &codec->literal_model,
               &codec->literal_tans_ready) &&
           init_tans_model_if_used(
               &codec->length_tans_model,
               &codec->length_model,
               &codec->length_tans_ready) &&
           init_tans_model_if_used(
               &codec->distance_tans_model,
               &codec->distance_model,
               &codec->distance_tans_ready);
}

static void destroy_tans_models(LzssTansCodec *codec)
{
    if (codec->literal_length_tans_ready) {
        tans_model_destroy(&codec->literal_length_tans_model);
    }

    if (codec->literal_tans_ready) {
        tans_model_destroy(&codec->literal_tans_model);
    }

    if (codec->length_tans_ready) {
        tans_model_destroy(&codec->length_tans_model);
    }

    if (codec->distance_tans_ready) {
        tans_model_destroy(&codec->distance_tans_model);
    }

    codec->literal_length_tans_ready = false;
    codec->literal_tans_ready = false;
    codec->length_tans_ready = false;
    codec->distance_tans_ready = false;
}

static bool encode_literal_length_extras(
    struct bio *extra_bio,
    const LzssSequence *sequence)
{
    size_t literal_length_symbol = 0;
    const DeflateClass *literal_length_class = nullptr;

    if (!find_class_for_value(
            LITERAL_LENGTH_CLASSES,
            array_count(LITERAL_LENGTH_CLASSES),
            0,
            UINT32_MAX,
            sequence->lit_length,
            &literal_length_symbol,
            &literal_length_class)) {
        return false;
    }

    (void)literal_length_symbol;
    bio_write_bits(
        extra_bio,
        sequence->lit_length - literal_length_class->base,
        literal_length_class->extra_bits
    );

    return true;
}

static bool encode_match_extras(
    LzssTansCodec *codec,
    struct bio *extra_bio,
    const LzssSequence *sequence)
{
    size_t length_symbol = 0;
    const DeflateClass *length_class = nullptr;

    if (!find_class_for_value(
            LENGTH_CLASSES,
            array_count(LENGTH_CLASSES),
            static_cast<uint32_t>(codec->config.min_match_length),
            static_cast<uint32_t>(codec->config.max_match_length),
            sequence->match_length,
            &length_symbol,
            &length_class)) {
        return false;
    }

    (void)length_symbol;
    bio_write_bits(
        extra_bio,
        sequence->match_length - length_class->base,
        length_class->extra_bits
    );

    size_t distance_symbol = 0;
    const DeflateClass *distance_class = nullptr;

    if (!find_class_for_value(
            DISTANCE_CLASSES,
            array_count(DISTANCE_CLASSES),
            1,
            static_cast<uint32_t>(codec->config.window_size),
            sequence->match_distance,
            &distance_symbol,
            &distance_class)) {
        return false;
    }

    (void)distance_symbol;
    bio_write_bits(
        extra_bio,
        sequence->match_distance - distance_class->base,
        distance_class->extra_bits
    );

    return true;
}

static bool encode_literal_length_symbol_reverse(
    LzssTansCodec *codec,
    struct tans_state *state,
    std::vector<struct tans_bit_chunk> *chunks,
    const LzssSequence *sequence)
{
    if (!codec->literal_length_tans_ready) {
        return false;
    }

    size_t literal_length_symbol = 0;
    const DeflateClass *literal_length_class = nullptr;

    if (!find_class_for_value(
            LITERAL_LENGTH_CLASSES,
            array_count(LITERAL_LENGTH_CLASSES),
            0,
            UINT32_MAX,
            sequence->lit_length,
            &literal_length_symbol,
            &literal_length_class)) {
        return false;
    }

    (void)literal_length_class;
    chunks->push_back(
        tans_encode_symbol_to_chunk(
            state,
            literal_length_symbol,
            &codec->literal_length_tans_model
        )
    );

    return true;
}

static bool encode_match_symbols_reverse(
    LzssTansCodec *codec,
    struct tans_state *state,
    std::vector<struct tans_bit_chunk> *chunks,
    const LzssSequence *sequence)
{
    size_t length_symbol = 0;
    const DeflateClass *length_class = nullptr;

    if (!codec->length_tans_ready ||
        !find_class_for_value(
            LENGTH_CLASSES,
            array_count(LENGTH_CLASSES),
            static_cast<uint32_t>(codec->config.min_match_length),
            static_cast<uint32_t>(codec->config.max_match_length),
            sequence->match_length,
            &length_symbol,
            &length_class)) {
        return false;
    }

    size_t distance_symbol = 0;
    const DeflateClass *distance_class = nullptr;

    if (!codec->distance_tans_ready ||
        !find_class_for_value(
            DISTANCE_CLASSES,
            array_count(DISTANCE_CLASSES),
            1,
            static_cast<uint32_t>(codec->config.window_size),
            sequence->match_distance,
            &distance_symbol,
            &distance_class)) {
        return false;
    }

    (void)length_class;
    (void)distance_class;
    chunks->push_back(
        tans_encode_symbol_to_chunk(
            state,
            distance_symbol,
            &codec->distance_tans_model
        )
    );
    chunks->push_back(
        tans_encode_symbol_to_chunk(
            state,
            length_symbol,
            &codec->length_tans_model
        )
    );

    return true;
}

static bool decode_literal_length_symbol(
    LzssTansCodec *codec,
    struct tans_state *state,
    struct bio *tans_bio,
    struct bio *extra_bio,
    uint32_t *out_length)
{
    if (!codec->literal_length_tans_ready) {
        return false;
    }

    const size_t literal_length_symbol =
        tans_decode_symbol(
            state,
            tans_bio,
            &codec->literal_length_tans_model
        );

    if (literal_length_symbol >= codec->literal_length_model.count) {
        return false;
    }

    const DeflateClass *literal_length_class = nullptr;

    if (!find_class_by_symbol(
            LITERAL_LENGTH_CLASSES,
            array_count(LITERAL_LENGTH_CLASSES),
            0,
            UINT32_MAX,
            literal_length_symbol,
            &literal_length_class)) {
        return false;
    }

    const uint32_t extra_value = literal_length_class->extra_bits == 0
        ? 0
        : bio_read_bits(extra_bio, literal_length_class->extra_bits);

    if (extra_value >= literal_length_class->size) {
        return false;
    }

    *out_length = literal_length_class->base + extra_value;
    return true;
}

static bool decode_length_symbol(
    LzssTansCodec *codec,
    struct tans_state *state,
    struct bio *tans_bio,
    struct bio *extra_bio,
    uint32_t *out_length)
{
    if (!codec->length_tans_ready) {
        return false;
    }

    const size_t length_symbol =
        tans_decode_symbol(state, tans_bio, &codec->length_tans_model);

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

    const uint32_t extra_value = length_class->extra_bits == 0
        ? 0
        : bio_read_bits(extra_bio, length_class->extra_bits);

    if (extra_value >= length_class->size) {
        return false;
    }

    const uint32_t length = length_class->base + extra_value;

    if (length < codec->config.min_match_length ||
        length > codec->config.max_match_length) {
        return false;
    }

    *out_length = length;
    return true;
}

static bool decode_distance_symbol(
    LzssTansCodec *codec,
    struct tans_state *state,
    struct bio *tans_bio,
    struct bio *extra_bio,
    uint32_t *out_distance)
{
    if (!codec->distance_tans_ready) {
        return false;
    }

    const size_t distance_symbol =
        tans_decode_symbol(state, tans_bio, &codec->distance_tans_model);

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

    const uint32_t extra_value = distance_class->extra_bits == 0
        ? 0
        : bio_read_bits(extra_bio, distance_class->extra_bits);

    if (extra_value >= distance_class->size) {
        return false;
    }

    const uint32_t distance = distance_class->base + extra_value;

    if (distance == 0 || distance > codec->config.window_size) {
        return false;
    }

    *out_distance = distance;
    return true;
}

bool lzss_tans_codec_init(
    LzssTansCodec *codec,
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

    const size_t literal_length_class_count =
        active_class_count(
            LITERAL_LENGTH_CLASSES,
            array_count(LITERAL_LENGTH_CLASSES),
            0,
            UINT32_MAX
        );

    codec->literal_length_extra_bit_count =
        max_extra_bit_count(
            LITERAL_LENGTH_CLASSES,
            array_count(LITERAL_LENGTH_CLASSES),
            0,
            UINT32_MAX
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

    model_create(&codec->literal_length_model, literal_length_class_count);
    model_create(&codec->literal_model, 256);
    model_create(&codec->length_model, length_class_count);
    model_create(&codec->distance_model, distance_class_count);

    return true;
}

void lzss_tans_codec_destroy(
    LzssTansCodec *codec)
{
    if (codec == nullptr) {
        return;
    }

    destroy_tans_models(codec);

    if (codec->literal_length_model.table != nullptr) {
        model_destroy(&codec->literal_length_model);
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

    std::memset(codec, 0, sizeof(*codec));
}

bool lzss_tans_encode_stream(
    LzssTansCodec *codec,
    struct bio *bio,
    const LzssSequenceStream *stream)
{
    if (codec == nullptr ||
        bio == nullptr ||
        stream == nullptr) {
        return false;
    }

    uint32_t sequence_count_u32 = 0;
    if (!checked_size_to_u32(stream->sequences.size(), &sequence_count_u32)) {
        return false;
    }

    if (!collect_static_model_stats(codec, stream) ||
        !write_u32(bio, sequence_count_u32) ||
        !write_static_model_header(bio, codec) ||
        !init_all_tans_models(codec)) {
        return false;
    }

    size_t symbol_count = stream->sequences.size();
    size_t extra_bit_capacity = 32;

    for (size_t i = 0; i < stream->sequences.size(); ++i) {
        const LzssSequence& sequence = stream->sequences[i];

        if (symbol_count >
            std::numeric_limits<size_t>::max() - sequence.lit_length) {
            return false;
        }
        symbol_count += sequence.lit_length;

        if (extra_bit_capacity >
            std::numeric_limits<size_t>::max() -
                codec->literal_length_extra_bit_count) {
            return false;
        }
        extra_bit_capacity += codec->literal_length_extra_bit_count;

        if (i == 0) {
            continue;
        }

        if (symbol_count >
            std::numeric_limits<size_t>::max() - 2) {
            return false;
        }
        symbol_count += 2;

        const size_t match_extra_bits =
            codec->length_extra_bit_count +
            codec->distance_extra_bit_count;

        if (extra_bit_capacity >
            std::numeric_limits<size_t>::max() - match_extra_bits) {
            return false;
        }
        extra_bit_capacity += match_extra_bits;
    }

    const size_t extra_word_capacity =
        std::max<size_t>(1, (extra_bit_capacity + 31) / 32 + 1);

    std::vector<struct tans_bit_chunk> tans_chunks;
    tans_chunks.reserve(symbol_count);
    std::vector<uint32_t> extra_words(extra_word_capacity, 0);

    struct bio extra_writer{};

    bio_open(
        &extra_writer,
        extra_words.data(),
        extra_words.data() + extra_words.size(),
        BIO_MODE_WRITE
    );

    for (size_t i = 0; i < stream->sequences.size(); ++i) {
        const LzssSequence *sequence = &stream->sequences[i];

        if (i > 0 &&
            !encode_match_extras(codec, &extra_writer, sequence)) {
            return false;
        }

        if (!encode_literal_length_extras(&extra_writer, sequence)) {
            return false;
        }
    }

    struct tans_state state{};
    tans_encode_init(&state);

    for (size_t i = stream->sequences.size(); i > 0; --i) {
        const size_t sequence_index = i - 1;
        const LzssSequence *sequence = &stream->sequences[sequence_index];

        for (uint32_t literal_index = sequence->lit_length;
             literal_index > 0;
             --literal_index) {
            if (!codec->literal_tans_ready) {
                return false;
            }

            const uint8_t literal =
                sequence->literals_ptr[literal_index - 1];

            tans_chunks.push_back(
                tans_encode_symbol_to_chunk(
                    &state,
                    literal,
                    &codec->literal_tans_model
                )
            );
        }

        if (!encode_literal_length_symbol_reverse(
                codec,
                &state,
                &tans_chunks,
                sequence)) {
            return false;
        }

        if (sequence_index > 0 &&
            !encode_match_symbols_reverse(
                codec,
                &state,
                &tans_chunks,
                sequence)) {
            return false;
        }
    }

    bio_close(&extra_writer, BIO_MODE_WRITE);

    size_t tans_bit_count = 12;
    for (const struct tans_bit_chunk& chunk : tans_chunks) {
        tans_bit_count += chunk.count;
    }

    std::vector<uint32_t> tans_words((tans_bit_count + 31) / 32 + 1, 0);
    struct bio tans_writer{};

    bio_open(
        &tans_writer,
        tans_words.data(),
        tans_words.data() + tans_words.size(),
        BIO_MODE_WRITE
    );

    tans_encode_flush(&state, &tans_writer);

    for (size_t i = tans_chunks.size(); i > 0; --i) {
        const struct tans_bit_chunk& chunk = tans_chunks[i - 1];

        if (chunk.count != 0) {
            bio_write_bits(&tans_writer, chunk.bits, chunk.count);
        }
    }

    bio_close(&tans_writer, BIO_MODE_WRITE);

    const size_t tans_word_count =
        static_cast<size_t>(tans_writer.ptr - tans_words.data());
    const size_t extra_word_count =
        static_cast<size_t>(extra_writer.ptr - extra_words.data());

    uint32_t tans_word_count_u32 = 0;
    uint32_t extra_word_count_u32 = 0;

    if (!checked_size_to_u32(tans_word_count, &tans_word_count_u32) ||
        !checked_size_to_u32(extra_word_count, &extra_word_count_u32) ||
        !write_u32(bio, tans_word_count_u32) ||
        !write_u32(bio, extra_word_count_u32)) {
        return false;
    }

    for (size_t i = 0; i < tans_word_count; ++i) {
        write_u32(bio, tans_words[i]);
    }

    for (size_t i = 0; i < extra_word_count; ++i) {
        write_u32(bio, extra_words[i]);
    }

    return true;
}

bool lzss_tans_decode_stream(
    LzssTansCodec *codec,
    struct bio *bio,
    LzssSequenceStream *out_stream)
{
    if (codec == nullptr ||
        bio == nullptr ||
        out_stream == nullptr) {
        return false;
    }

    const uint32_t sequence_count = read_u32(bio);

    if (!read_static_model_header(bio, codec) ||
        !init_all_tans_models(codec)) {
        return false;
    }

    const uint32_t tans_word_count = read_u32(bio);
    const uint32_t extra_word_count = read_u32(bio);

    if (tans_word_count == 0) {
        return false;
    }

    std::vector<uint32_t> tans_words(tans_word_count, 0);
    std::vector<uint32_t> extra_words(extra_word_count, 0);

    for (uint32_t i = 0; i < tans_word_count; ++i) {
        tans_words[i] = read_u32(bio);
    }

    for (uint32_t i = 0; i < extra_word_count; ++i) {
        extra_words[i] = read_u32(bio);
    }

    struct bio tans_reader{};
    struct bio extra_reader{};

    bio_open(
        &tans_reader,
        tans_words.data(),
        tans_words.data() + tans_words.size(),
        BIO_MODE_READ
    );

    bio_open(
        &extra_reader,
        extra_words.data(),
        extra_words.data() + extra_words.size(),
        BIO_MODE_READ
    );

    struct tans_state state{};
    tans_decode_init(&state, &tans_reader);

    out_stream->sequences.clear();
    out_stream->literals.clear();
    out_stream->sequences.reserve(sequence_count);

    std::vector<size_t> literal_offsets;
    literal_offsets.reserve(sequence_count);

    for (uint32_t i = 0; i < sequence_count; ++i) {
        LzssSequence sequence{};

        if (i > 0) {
            if (!decode_length_symbol(
                    codec,
                    &state,
                    &tans_reader,
                    &extra_reader,
                    &sequence.match_length)) {
                return false;
            }

            if (!decode_distance_symbol(
                    codec,
                    &state,
                    &tans_reader,
                    &extra_reader,
                    &sequence.match_distance)) {
                return false;
            }
        }

        if (!decode_literal_length_symbol(
                codec,
                &state,
                &tans_reader,
                &extra_reader,
                &sequence.lit_length)) {
            return false;
        }

        const size_t literal_offset = out_stream->literals.size();
        literal_offsets.push_back(literal_offset);

        if (out_stream->literals.size() >
            std::numeric_limits<size_t>::max() - sequence.lit_length) {
            return false;
        }

        out_stream->literals.resize(literal_offset + sequence.lit_length);

        for (uint32_t literal_index = 0;
             literal_index < sequence.lit_length;
             ++literal_index) {
            if (!codec->literal_tans_ready) {
                return false;
            }

            const size_t literal =
                tans_decode_symbol(
                    &state,
                    &tans_reader,
                    &codec->literal_tans_model
                );

            if (literal > UINT8_MAX) {
                return false;
            }

            out_stream->literals[literal_offset + literal_index] =
                static_cast<uint8_t>(literal);
        }

        sequence_stream_push(out_stream, sequence);
    }

    const uint8_t *literal_base = out_stream->literals.data();
    for (size_t i = 0; i < out_stream->sequences.size(); ++i) {
        LzssSequence& sequence = out_stream->sequences[i];
        sequence.literals_ptr =
            sequence.lit_length == 0
                ? nullptr
                : literal_base + literal_offsets[i];
    }

    return sequence_layout_is_valid(codec, out_stream);
}

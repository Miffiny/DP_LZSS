#include "adaptive_ac.h"

#include "ac.h"
#include "bio.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

static constexpr size_t REP_DISTANCE_COUNT = 3;

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
    {0, 1, 0},              {1, 1, 0},
    {2, 2, 1},              {4, 4, 2},
    {8, 8, 3},              {16, 16, 4},
    {32, 32, 5},            {64, 64, 6},
    {128, 128, 7},          {256, 256, 8},
    {512, 512, 9},          {1024, 1024, 10},
    {2048, 2048, 11},       {4096, 4096, 12},
    {8192, 8192, 13},       {16384, 16384, 14},
    {32768, 32768, 15},     {65536, 65536, 16},
    {131072, 131072, 17},   {262144, 262144, 18},
    {524288, 524288, 19},   {1048576, 1048576, 20},
    {2097152, 2097152, 21}, {4194304, 4194304, 22},
    {8388608, 8388608, 23}, {16777216, 16777216, 24},
    {33554432, 33554432, 25}, {67108864, 67108864, 26},
    {134217728, 134217728, 27}, {268435456, 268435456, 28},
    {536870912, 536870912, 29}, {1073741824, 1073741824, 30},
    {2147483648u, 2147483648u, 31},
};

static const DeflateClass DISTANCE_CLASSES[] = {
    {1, 1, 0},        {2, 1, 0},        {3, 1, 0},
    {4, 1, 0},        {5, 2, 1},        {7, 2, 1},
    {9, 4, 2},        {13, 4, 2},       {17, 8, 3},
    {25, 8, 3},       {33, 16, 4},      {49, 16, 4},
    {65, 32, 5},      {97, 32, 5},      {129, 64, 6},
    {193, 64, 6},     {257, 128, 7},    {385, 128, 7},
    {513, 256, 8},    {769, 256, 8},    {1025, 512, 9},
    {1537, 512, 9},   {2049, 1024, 10}, {3073, 1024, 10},
    {4097, 2048, 11}, {6145, 2048, 11},
    {8193, 4096, 12}, {12289, 4096, 12},
    {16385, 8192, 13}, {24577, 8192, 13},
    {32769, 16384, 14}, {49153, 16384, 14},
    {65537, 32768, 15}, {98305, 32768, 15},
    {131073, 65536, 16}, {196609, 65536, 16},
    {262145, 131072, 17}, {393217, 131072, 17},
    {524289, 262144, 18}, {786433, 262144, 18},
};

struct RepeatDistanceState {
    uint32_t distances[REP_DISTANCE_COUNT];
};

struct Order1ContextState {
    size_t literal_length_context;
    size_t literal_context;
    size_t length_context;
    size_t distance_context;
};

struct DistanceBitTreeModels {
    std::unordered_map<uint32_t, struct model> models;
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
    if (out_symbol == nullptr || out_class == nullptr) {
        return false;
    }

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
    if (out_class == nullptr) {
        return false;
    }

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

static size_t distance_symbol_count(uint32_t window_size)
{
    return REP_DISTANCE_COUNT +
           active_class_count(
               DISTANCE_CLASSES,
               array_count(DISTANCE_CLASSES),
               1,
               window_size
           );
}

static bool distance_uses_bit_tree(const LzssAdaptiveAcCodec *codec)
{
    return codec != nullptr &&
           codec->config.distance_coding == LZSS_DISTANCE_BIT_TREE;
}

static size_t distance_mode_symbol_count(
    const LzssConfig *config)
{
    if (config->distance_coding == LZSS_DISTANCE_BIT_TREE) {
        return REP_DISTANCE_COUNT + 1;
    }

    return distance_symbol_count(static_cast<uint32_t>(config->window_size));
}

static size_t distance_bit_count_for_window(size_t window_size)
{
    if (window_size <= 1) {
        return 0;
    }

    size_t bit_count = 0;
    size_t max_value = window_size - 1;
    while (max_value > 0) {
        ++bit_count;
        max_value >>= 1;
    }

    return bit_count;
}

static void repeat_distance_state_init(RepeatDistanceState *state)
{
    if (state == nullptr) {
        return;
    }

    for (uint32_t& distance : state->distances) {
        distance = 0;
    }
}

static void repeat_distance_array_init(uint32_t *distances)
{
    if (distances == nullptr) {
        return;
    }

    for (size_t i = 0; i < REP_DISTANCE_COUNT; ++i) {
        distances[i] = 0;
    }
}

static bool find_repeat_distance_symbol_array(
    const uint32_t *distances,
    uint32_t distance,
    size_t *out_symbol)
{
    if (distances == nullptr || out_symbol == nullptr || distance == 0) {
        return false;
    }

    for (size_t i = 0; i < REP_DISTANCE_COUNT; ++i) {
        if (distances[i] == distance) {
            *out_symbol = i;
            return true;
        }
    }

    return false;
}

static bool find_repeat_distance_symbol(
    const RepeatDistanceState *state,
    uint32_t distance,
    size_t *out_symbol)
{
    if (state == nullptr) {
        return false;
    }

    return find_repeat_distance_symbol_array(
        state->distances,
        distance,
        out_symbol
    );
}

static void update_repeat_distance_array(
    uint32_t *distances,
    uint32_t distance)
{
    if (distances == nullptr || distance == 0) {
        return;
    }

    size_t existing_index = REP_DISTANCE_COUNT;
    for (size_t i = 0; i < REP_DISTANCE_COUNT; ++i) {
        if (distances[i] == distance) {
            existing_index = i;
            break;
        }
    }

    const size_t shift_count =
        existing_index == REP_DISTANCE_COUNT
            ? REP_DISTANCE_COUNT - 1
            : existing_index;

    for (size_t i = shift_count; i > 0; --i) {
        distances[i] = distances[i - 1];
    }

    distances[0] = distance;
}

static void update_repeat_distances(
    RepeatDistanceState *state,
    uint32_t distance)
{
    if (state == nullptr) {
        return;
    }

    update_repeat_distance_array(state->distances, distance);
}

static bool distance_symbol_for_value(
    const LzssAdaptiveAcCodec *codec,
    const RepeatDistanceState *repeat_state,
    uint32_t distance,
    size_t *out_symbol,
    const DeflateClass **out_class)
{
    if (codec == nullptr ||
        repeat_state == nullptr ||
        out_symbol == nullptr ||
        out_class == nullptr) {
        return false;
    }

    size_t repeat_symbol = 0;
    if (find_repeat_distance_symbol(
            repeat_state,
            distance,
            &repeat_symbol)) {
        *out_symbol = repeat_symbol;
        *out_class = nullptr;
        return true;
    }

    size_t distance_class_symbol = 0;
    if (!find_class_for_value(
            DISTANCE_CLASSES,
            array_count(DISTANCE_CLASSES),
            1,
            static_cast<uint32_t>(codec->config.window_size),
            distance,
            &distance_class_symbol,
            out_class)) {
        return false;
    }

    *out_symbol = REP_DISTANCE_COUNT + distance_class_symbol;
    return true;
}

static bool distance_value_for_symbol(
    const LzssAdaptiveAcCodec *codec,
    const RepeatDistanceState *repeat_state,
    size_t symbol,
    const DeflateClass **out_class,
    uint32_t *out_distance)
{
    if (codec == nullptr ||
        repeat_state == nullptr ||
        out_class == nullptr ||
        out_distance == nullptr) {
        return false;
    }

    if (symbol < REP_DISTANCE_COUNT) {
        const uint32_t distance = repeat_state->distances[symbol];
        if (distance == 0 || distance > codec->config.window_size) {
            return false;
        }

        *out_class = nullptr;
        *out_distance = distance;
        return true;
    }

    const DeflateClass *distance_class = nullptr;
    if (!find_class_by_symbol(
            DISTANCE_CLASSES,
            array_count(DISTANCE_CLASSES),
            1,
            static_cast<uint32_t>(codec->config.window_size),
            symbol - REP_DISTANCE_COUNT,
            &distance_class)) {
        return false;
    }

    *out_class = distance_class;
    *out_distance = 0;
    return true;
}

static bool checked_size_to_u32(size_t value, uint32_t *out_value)
{
    if (out_value == nullptr || value > UINT32_MAX) {
        return false;
    }

    *out_value = static_cast<uint32_t>(value);
    return true;
}

static bool write_u32(struct bio *bio, uint32_t value)
{
    if (bio == nullptr) {
        return false;
    }

    bio_write_bits(bio, value, 32);
    return true;
}

static uint32_t read_u32(struct bio *bio)
{
    return bio_read_bits(bio, 32);
}

static bool create_bit_models(struct model **models, size_t count)
{
    if (models == nullptr) {
        return false;
    }

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

static bool create_context_models(
    struct model **models,
    size_t context_count,
    size_t symbol_count)
{
    if (models == nullptr || context_count == 0 || symbol_count == 0) {
        return false;
    }

    *models = static_cast<struct model *>(
        std::calloc(context_count, sizeof(struct model))
    );

    if (*models == nullptr) {
        return false;
    }

    for (size_t i = 0; i < context_count; ++i) {
        model_create(&(*models)[i], symbol_count);
    }

    return true;
}

static void destroy_context_models(struct model *models, size_t context_count)
{
    if (models == nullptr) {
        return;
    }

    for (size_t i = 0; i < context_count; ++i) {
        if (models[i].table != nullptr) {
            model_destroy(&models[i]);
        }
    }

    std::free(models);
}

static bool create_distance_bit_tree_models(LzssAdaptiveAcCodec *codec)
{
    if (codec == nullptr) {
        return false;
    }

    codec->distance_bit_tree_models =
        new (std::nothrow) DistanceBitTreeModels();
    return codec->distance_bit_tree_models != nullptr;
}

static void destroy_distance_bit_tree_models(LzssAdaptiveAcCodec *codec)
{
    if (codec == nullptr || codec->distance_bit_tree_models == nullptr) {
        return;
    }

    auto *tree = static_cast<DistanceBitTreeModels *>(
        codec->distance_bit_tree_models
    );

    for (auto& entry : tree->models) {
        if (entry.second.table != nullptr) {
            model_destroy(&entry.second);
        }
    }

    delete tree;
    codec->distance_bit_tree_models = nullptr;
}

static struct model *distance_bit_tree_model_for_node(
    LzssAdaptiveAcCodec *codec,
    uint32_t node_index)
{
    if (codec == nullptr || codec->distance_bit_tree_models == nullptr) {
        return nullptr;
    }

    auto *tree = static_cast<DistanceBitTreeModels *>(
        codec->distance_bit_tree_models
    );

    auto result = tree->models.try_emplace(node_index);
    struct model *model = &result.first->second;
    if (result.second) {
        model_create(model, 2);
    }

    return model;
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

    if (config->distance_coding != LZSS_DISTANCE_CLASS &&
        config->distance_coding != LZSS_DISTANCE_BIT_TREE) {
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
    const LzssAdaptiveAcCodec *codec,
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
    const LzssAdaptiveAcCodec *codec,
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

static void order1_context_state_init(Order1ContextState *state)
{
    if (state == nullptr) {
        return;
    }

    state->literal_length_context = 0;
    state->literal_context = 0;
    state->length_context = 0;
    state->distance_context = 0;
}

static void encode_symbol_adaptive(
    struct ac *ac,
    struct bio *bio,
    size_t symbol,
    struct model *model)
{
    ac_encode_symbol_model(ac, bio, symbol, model);
    model_update(model, symbol);
}

static bool decode_symbol_adaptive(
    struct ac *ac,
    struct bio *bio,
    struct model *model,
    size_t *out_symbol)
{
    if (out_symbol == nullptr || model == nullptr || model->total == 0) {
        return false;
    }

    const size_t symbol = ac_decode_symbol_model(ac, bio, model);
    if (symbol >= model->count) {
        return false;
    }

    model_update(model, symbol);
    *out_symbol = symbol;
    return true;
}

static bool encode_symbol_order1(
    struct ac *ac,
    struct bio *bio,
    size_t symbol,
    struct model *models,
    size_t context_count,
    size_t *context)
{
    if (models == nullptr ||
        context == nullptr ||
        *context >= context_count) {
        return false;
    }

    struct model *model = &models[*context];
    if (symbol >= model->count) {
        return false;
    }

    encode_symbol_adaptive(ac, bio, symbol, model);
    *context = symbol + 1;
    return true;
}

static bool decode_symbol_order1(
    struct ac *ac,
    struct bio *bio,
    struct model *models,
    size_t context_count,
    size_t *context,
    size_t *out_symbol)
{
    if (models == nullptr ||
        context == nullptr ||
        out_symbol == nullptr ||
        *context >= context_count) {
        return false;
    }

    size_t symbol = 0;
    if (!decode_symbol_adaptive(ac, bio, &models[*context], &symbol)) {
        return false;
    }

    *context = symbol + 1;
    *out_symbol = symbol;
    return true;
}

static void encode_extra_bits_noise(
    struct ac *ac,
    struct bio *bio,
    struct model *bit_models,
    size_t bit_count,
    uint32_t value)
{
    for (size_t bit_index = 0; bit_index < bit_count; ++bit_index) {
        const size_t shift = bit_count - 1 - bit_index;
        const size_t bit = (value >> shift) & 1u;
        ac_encode_symbol_model(ac, bio, bit, &bit_models[bit_index]);
    }
}

static bool decode_extra_bits_noise(
    struct ac *ac,
    struct bio *bio,
    struct model *bit_models,
    size_t bit_count,
    uint32_t *out_value)
{
    if (out_value == nullptr) {
        return false;
    }

    uint32_t value = 0;
    for (size_t bit_index = 0; bit_index < bit_count; ++bit_index) {
        if (bit_models[bit_index].total == 0) {
            return false;
        }

        const size_t bit =
            ac_decode_symbol_model(ac, bio, &bit_models[bit_index]);
        if (bit > 1) {
            return false;
        }

        value = (value << 1) | static_cast<uint32_t>(bit);
    }

    *out_value = value;
    return true;
}

static bool encode_distance_bit_tree(
    LzssAdaptiveAcCodec *codec,
    struct ac *ac,
    struct bio *bio,
    uint32_t distance)
{
    if (codec == nullptr ||
        ac == nullptr ||
        bio == nullptr ||
        distance == 0 ||
        distance > codec->config.window_size) {
        return false;
    }

    const uint32_t value = distance - 1;
    uint32_t node_index = 0;

    for (size_t bit_index = 0;
         bit_index < codec->distance_bit_count;
         ++bit_index) {
        struct model *bit_model =
            distance_bit_tree_model_for_node(codec, node_index);
        if (bit_model == nullptr) {
            return false;
        }

        const size_t shift = codec->distance_bit_count - 1 - bit_index;
        const size_t bit = (value >> shift) & 1u;
        encode_symbol_adaptive(ac, bio, bit, bit_model);
        node_index = node_index * 2 + 1 + static_cast<uint32_t>(bit);
    }

    return true;
}

static bool decode_distance_bit_tree(
    LzssAdaptiveAcCodec *codec,
    struct ac *ac,
    struct bio *bio,
    uint32_t *out_distance)
{
    if (codec == nullptr ||
        ac == nullptr ||
        bio == nullptr ||
        out_distance == nullptr) {
        return false;
    }

    uint32_t value = 0;
    uint32_t node_index = 0;

    for (size_t bit_index = 0;
         bit_index < codec->distance_bit_count;
         ++bit_index) {
        struct model *bit_model =
            distance_bit_tree_model_for_node(codec, node_index);
        if (bit_model == nullptr) {
            return false;
        }

        size_t bit = 0;
        if (!decode_symbol_adaptive(ac, bio, bit_model, &bit) ||
            bit > 1) {
            return false;
        }

        value = (value << 1) | static_cast<uint32_t>(bit);
        node_index = node_index * 2 + 1 + static_cast<uint32_t>(bit);
    }

    const uint32_t distance = value + 1;
    if (distance == 0 || distance > codec->config.window_size) {
        return false;
    }

    *out_distance = distance;
    return true;
}

static bool encode_literal_length(
    LzssAdaptiveAcCodec *codec,
    struct ac *ac,
    struct bio *bio,
    Order1ContextState *context_state,
    uint32_t literal_length)
{
    if (context_state == nullptr) {
        return false;
    }

    size_t symbol = 0;
    const DeflateClass *literal_length_class = nullptr;
    if (!find_class_for_value(
            LITERAL_LENGTH_CLASSES,
            array_count(LITERAL_LENGTH_CLASSES),
            0,
            UINT32_MAX,
            literal_length,
            &symbol,
            &literal_length_class)) {
        return false;
    }

    if (!encode_symbol_order1(
            ac,
            bio,
            symbol,
            codec->literal_length_models,
            codec->literal_length_context_count,
            &context_state->literal_length_context)) {
        return false;
    }

    encode_extra_bits_noise(
        ac,
        bio,
        codec->literal_length_extra_bit_models,
        literal_length_class->extra_bits,
        literal_length - literal_length_class->base
    );
    return true;
}

static bool decode_literal_length(
    LzssAdaptiveAcCodec *codec,
    struct ac *ac,
    struct bio *bio,
    Order1ContextState *context_state,
    uint32_t *out_literal_length)
{
    if (context_state == nullptr) {
        return false;
    }

    size_t symbol = 0;
    if (!decode_symbol_order1(
            ac,
            bio,
            codec->literal_length_models,
            codec->literal_length_context_count,
            &context_state->literal_length_context,
            &symbol)) {
        return false;
    }

    const DeflateClass *literal_length_class = nullptr;
    if (!find_class_by_symbol(
            LITERAL_LENGTH_CLASSES,
            array_count(LITERAL_LENGTH_CLASSES),
            0,
            UINT32_MAX,
            symbol,
            &literal_length_class)) {
        return false;
    }

    uint32_t extra_value = 0;
    if (!decode_extra_bits_noise(
            ac,
            bio,
            codec->literal_length_extra_bit_models,
            literal_length_class->extra_bits,
            &extra_value) ||
        extra_value >= literal_length_class->size) {
        return false;
    }

    *out_literal_length = literal_length_class->base + extra_value;
    return true;
}

static bool encode_match(
    LzssAdaptiveAcCodec *codec,
    struct ac *ac,
    struct bio *bio,
    RepeatDistanceState *repeat_state,
    Order1ContextState *context_state,
    const LzssSequence *sequence)
{
    if (codec == nullptr ||
        repeat_state == nullptr ||
        context_state == nullptr ||
        sequence == nullptr) {
        return false;
    }

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

    if (!encode_symbol_order1(
            ac,
            bio,
            length_symbol,
            codec->length_models,
            codec->length_context_count,
            &context_state->length_context)) {
        return false;
    }

    encode_extra_bits_noise(
        ac,
        bio,
        codec->length_extra_bit_models,
        length_class->extra_bits,
        sequence->match_length - length_class->base
    );

    if (distance_uses_bit_tree(codec)) {
        size_t distance_symbol = REP_DISTANCE_COUNT;
        const bool is_repeat_distance =
            find_repeat_distance_symbol(
                repeat_state,
                sequence->match_distance,
                &distance_symbol
            );

        if (!encode_symbol_order1(
                ac,
                bio,
                distance_symbol,
                codec->distance_models,
                codec->distance_context_count,
                &context_state->distance_context)) {
            return false;
        }

        if (!is_repeat_distance &&
            !encode_distance_bit_tree(
                codec,
                ac,
                bio,
                sequence->match_distance)) {
            return false;
        }

        update_repeat_distances(repeat_state, sequence->match_distance);
        return true;
    }

    size_t distance_symbol = 0;
    const DeflateClass *distance_class = nullptr;
    if (!distance_symbol_for_value(
            codec,
            repeat_state,
            sequence->match_distance,
            &distance_symbol,
            &distance_class)) {
        return false;
    }

    if (!encode_symbol_order1(
            ac,
            bio,
            distance_symbol,
            codec->distance_models,
            codec->distance_context_count,
            &context_state->distance_context)) {
        return false;
    }

    if (distance_class != nullptr) {
        encode_extra_bits_noise(
            ac,
            bio,
            codec->distance_extra_bit_models,
            distance_class->extra_bits,
            sequence->match_distance - distance_class->base
        );
    }

    update_repeat_distances(repeat_state, sequence->match_distance);
    return true;
}

static bool decode_match(
    LzssAdaptiveAcCodec *codec,
    struct ac *ac,
    struct bio *bio,
    RepeatDistanceState *repeat_state,
    Order1ContextState *context_state,
    LzssSequence *sequence)
{
    if (codec == nullptr ||
        repeat_state == nullptr ||
        context_state == nullptr ||
        sequence == nullptr) {
        return false;
    }

    size_t length_symbol = 0;
    if (!decode_symbol_order1(
            ac,
            bio,
            codec->length_models,
            codec->length_context_count,
            &context_state->length_context,
            &length_symbol)) {
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

    uint32_t length_extra = 0;
    if (!decode_extra_bits_noise(
            ac,
            bio,
            codec->length_extra_bit_models,
            length_class->extra_bits,
            &length_extra) ||
        length_extra >= length_class->size) {
        return false;
    }

    const uint32_t length = length_class->base + length_extra;
    if (length < codec->config.min_match_length ||
        length > codec->config.max_match_length) {
        return false;
    }

    if (distance_uses_bit_tree(codec)) {
        size_t distance_symbol = 0;
        if (!decode_symbol_order1(
                ac,
                bio,
                codec->distance_models,
                codec->distance_context_count,
                &context_state->distance_context,
                &distance_symbol)) {
            return false;
        }

        uint32_t distance = 0;
        if (distance_symbol < REP_DISTANCE_COUNT) {
            distance = repeat_state->distances[distance_symbol];
            if (distance == 0 || distance > codec->config.window_size) {
                return false;
            }
        } else if (distance_symbol == REP_DISTANCE_COUNT) {
            if (!decode_distance_bit_tree(codec, ac, bio, &distance)) {
                return false;
            }
        } else {
            return false;
        }

        sequence->match_length = length;
        sequence->match_distance = distance;
        update_repeat_distances(repeat_state, distance);
        return true;
    }

    size_t distance_symbol = 0;
    if (!decode_symbol_order1(
            ac,
            bio,
            codec->distance_models,
            codec->distance_context_count,
            &context_state->distance_context,
            &distance_symbol)) {
        return false;
    }

    const DeflateClass *distance_class = nullptr;
    uint32_t distance = 0;
    if (!distance_value_for_symbol(
            codec,
            repeat_state,
            distance_symbol,
            &distance_class,
            &distance)) {
        return false;
    }

    if (distance_class != nullptr) {
        uint32_t distance_extra = 0;
        if (!decode_extra_bits_noise(
                ac,
                bio,
                codec->distance_extra_bit_models,
                distance_class->extra_bits,
                &distance_extra) ||
            distance_extra >= distance_class->size) {
            return false;
        }

        distance = distance_class->base + distance_extra;
    }

    if (distance == 0 || distance > codec->config.window_size) {
        return false;
    }

    sequence->match_length = length;
    sequence->match_distance = distance;
    update_repeat_distances(repeat_state, distance);
    return true;
}

static constexpr size_t ADAPTIVE_COST_MODEL_MAX_TOTAL = 1u << 20;

static size_t distance_bit_tree_node_count(size_t bit_count)
{
    if (bit_count == 0) {
        return 0;
    }

    if (bit_count >= std::numeric_limits<size_t>::digits) {
        return 0;
    }

    return (static_cast<size_t>(1) << bit_count) - 1;
}

static bool flat_model_shape_is_valid(size_t context_count, size_t symbol_count)
{
    return context_count != 0 &&
           symbol_count != 0 &&
           context_count <=
               std::numeric_limits<size_t>::max() / symbol_count;
}

static bool rescale_frequency_row(
    std::vector<uint32_t> *frequencies,
    std::vector<size_t> *totals,
    size_t symbol_count,
    size_t context)
{
    if (frequencies == nullptr ||
        totals == nullptr ||
        symbol_count == 0 ||
        context >= totals->size()) {
        return false;
    }

    const size_t base = context * symbol_count;
    if (base > frequencies->size() ||
        frequencies->size() - base < symbol_count) {
        return false;
    }

    size_t total = 0;
    for (size_t symbol = 0; symbol < symbol_count; ++symbol) {
        uint32_t frequency = ((*frequencies)[base + symbol] + 1) / 2;
        if (frequency == 0) {
            frequency = 1;
        }

        (*frequencies)[base + symbol] = frequency;
        if (total > std::numeric_limits<size_t>::max() - frequency) {
            return false;
        }
        total += frequency;
    }

    (*totals)[context] = total;
    return true;
}

static bool update_frequency(
    std::vector<uint32_t> *frequencies,
    std::vector<size_t> *totals,
    size_t symbol_count,
    size_t context,
    size_t symbol)
{
    if (frequencies == nullptr ||
        totals == nullptr ||
        symbol_count == 0 ||
        context >= totals->size() ||
        symbol >= symbol_count) {
        return false;
    }

    const size_t base = context * symbol_count;
    if (base > frequencies->size() ||
        frequencies->size() - base <= symbol) {
        return false;
    }

    uint32_t& frequency = (*frequencies)[base + symbol];
    if (frequency == UINT32_MAX) {
        return false;
    }

    ++frequency;
    if ((*totals)[context] == std::numeric_limits<size_t>::max()) {
        return false;
    }
    ++(*totals)[context];

    if ((*totals)[context] >= ADAPTIVE_COST_MODEL_MAX_TOTAL) {
        return rescale_frequency_row(
            frequencies,
            totals,
            symbol_count,
            context
        );
    }

    return true;
}

static bool costs_from_frequencies(
    const std::vector<uint32_t>& frequencies,
    const std::vector<size_t>& totals,
    size_t symbol_count,
    std::vector<double> *costs)
{
    if (costs == nullptr ||
        symbol_count == 0 ||
        totals.size() >
            std::numeric_limits<size_t>::max() / symbol_count ||
        frequencies.size() != totals.size() * symbol_count) {
        return false;
    }

    std::vector<double> working(frequencies.size(), 0.0);
    for (size_t context = 0; context < totals.size(); ++context) {
        const size_t total = totals[context];
        if (total == 0) {
            return false;
        }

        const size_t base = context * symbol_count;
        for (size_t symbol = 0; symbol < symbol_count; ++symbol) {
            const uint32_t frequency = frequencies[base + symbol];
            if (frequency == 0) {
                return false;
            }

            working[base + symbol] = std::log2(
                static_cast<double>(total) /
                static_cast<double>(frequency)
            );
        }
    }

    *costs = std::move(working);
    return true;
}

static bool lookup_symbol_cost(
    const std::vector<double>& costs,
    size_t context_count,
    size_t symbol_count,
    size_t context,
    size_t symbol,
    double *out_cost)
{
    if (out_cost == nullptr ||
        !flat_model_shape_is_valid(context_count, symbol_count) ||
        costs.size() != context_count * symbol_count ||
        context >= context_count ||
        symbol >= symbol_count) {
        return false;
    }

    const double cost = costs[context * symbol_count + symbol];
    if (!std::isfinite(cost)) {
        return false;
    }

    *out_cost = cost;
    return true;
}

static bool literal_length_symbol_for_value(
    size_t literal_length,
    size_t *out_symbol,
    const DeflateClass **out_class)
{
    if (literal_length > UINT32_MAX) {
        return false;
    }

    return find_class_for_value(
        LITERAL_LENGTH_CLASSES,
        array_count(LITERAL_LENGTH_CLASSES),
        0,
        UINT32_MAX,
        static_cast<uint32_t>(literal_length),
        out_symbol,
        out_class
    );
}

static bool literal_length_context_after(
    const LzssAdaptiveAcCostModel *cost_model,
    size_t literal_length_base_context,
    size_t literal_length,
    size_t *out_context)
{
    if (cost_model == nullptr ||
        out_context == nullptr ||
        literal_length_base_context >=
            cost_model->literal_length_context_count) {
        return false;
    }

    size_t symbol = 0;
    const DeflateClass *literal_length_class = nullptr;
    if (!literal_length_symbol_for_value(
            literal_length,
            &symbol,
            &literal_length_class) ||
        symbol >= cost_model->literal_length_symbol_count) {
        return false;
    }

    (void)literal_length_class;
    *out_context = symbol + 1;
    return *out_context < cost_model->literal_length_context_count;
}

static bool length_cost_for_value(
    const LzssAdaptiveAcCostModel *cost_model,
    size_t context,
    uint32_t length,
    size_t *out_symbol,
    double *out_cost)
{
    if (cost_model == nullptr ||
        out_symbol == nullptr ||
        out_cost == nullptr ||
        context >= cost_model->length_context_count) {
        return false;
    }

    size_t symbol = 0;
    const DeflateClass *length_class = nullptr;
    if (!find_class_for_value(
            LENGTH_CLASSES,
            array_count(LENGTH_CLASSES),
            static_cast<uint32_t>(cost_model->config.min_match_length),
            static_cast<uint32_t>(cost_model->config.max_match_length),
            length,
            &symbol,
            &length_class) ||
        symbol >= cost_model->length_symbol_count) {
        return false;
    }

    double symbol_cost = 0.0;
    if (!lookup_symbol_cost(
            cost_model->length_symbol_costs,
            cost_model->length_context_count,
            cost_model->length_symbol_count,
            context,
            symbol,
            &symbol_cost)) {
        return false;
    }

    *out_symbol = symbol;
    *out_cost = symbol_cost + static_cast<double>(length_class->extra_bits);
    return std::isfinite(*out_cost);
}

static bool distance_symbol_for_cost_state(
    const LzssAdaptiveAcCostModel *cost_model,
    const uint32_t *repeat_distances,
    uint32_t distance,
    size_t *out_symbol,
    const DeflateClass **out_class)
{
    if (cost_model == nullptr ||
        repeat_distances == nullptr ||
        out_symbol == nullptr ||
        out_class == nullptr ||
        distance == 0 ||
        distance > cost_model->config.window_size) {
        return false;
    }

    size_t repeat_symbol = 0;
    if (find_repeat_distance_symbol_array(
            repeat_distances,
            distance,
            &repeat_symbol)) {
        *out_symbol = repeat_symbol;
        *out_class = nullptr;
        return true;
    }

    if (cost_model->config.distance_coding == LZSS_DISTANCE_BIT_TREE) {
        *out_symbol = REP_DISTANCE_COUNT;
        *out_class = nullptr;
        return true;
    }

    size_t distance_class_symbol = 0;
    const DeflateClass *distance_class = nullptr;
    if (!find_class_for_value(
            DISTANCE_CLASSES,
            array_count(DISTANCE_CLASSES),
            1,
            static_cast<uint32_t>(cost_model->config.window_size),
            distance,
            &distance_class_symbol,
            &distance_class)) {
        return false;
    }

    *out_symbol = REP_DISTANCE_COUNT + distance_class_symbol;
    *out_class = distance_class;
    return true;
}

static bool update_distance_bit_tree_frequencies(
    const LzssAdaptiveAcCostModel *cost_model,
    uint32_t distance,
    std::vector<uint32_t> *frequencies,
    std::vector<size_t> *totals)
{
    if (cost_model == nullptr ||
        frequencies == nullptr ||
        totals == nullptr ||
        distance == 0 ||
        distance > cost_model->config.window_size) {
        return false;
    }

    const uint32_t value = distance - 1;
    size_t node_index = 0;

    for (size_t bit_index = 0;
         bit_index < cost_model->distance_bit_count;
         ++bit_index) {
        const size_t shift = cost_model->distance_bit_count - 1 - bit_index;
        const size_t bit = (value >> shift) & 1u;

        if (!update_frequency(frequencies, totals, 2, node_index, bit)) {
            return false;
        }

        node_index = node_index * 2 + 1 + bit;
    }

    return true;
}

static bool distance_bit_tree_cost(
    const LzssAdaptiveAcCostModel *cost_model,
    uint32_t distance,
    double *out_cost)
{
    if (cost_model == nullptr ||
        out_cost == nullptr ||
        distance == 0 ||
        distance > cost_model->config.window_size) {
        return false;
    }

    const uint32_t value = distance - 1;
    size_t node_index = 0;
    double total_cost = 0.0;

    for (size_t bit_index = 0;
         bit_index < cost_model->distance_bit_count;
         ++bit_index) {
        const size_t shift = cost_model->distance_bit_count - 1 - bit_index;
        const size_t bit = (value >> shift) & 1u;
        double bit_cost = 0.0;

        if (!lookup_symbol_cost(
                cost_model->distance_bit_costs,
                distance_bit_tree_node_count(cost_model->distance_bit_count),
                2,
                node_index,
                bit,
                &bit_cost)) {
            return false;
        }

        total_cost += bit_cost;
        node_index = node_index * 2 + 1 + bit;
    }

    *out_cost = total_cost;
    return std::isfinite(*out_cost);
}

static bool distance_cost_for_value(
    const LzssAdaptiveAcCostModel *cost_model,
    const uint32_t *repeat_distances,
    size_t context,
    uint32_t distance,
    size_t *out_symbol,
    double *out_cost)
{
    if (cost_model == nullptr ||
        repeat_distances == nullptr ||
        out_symbol == nullptr ||
        out_cost == nullptr ||
        context >= cost_model->distance_context_count) {
        return false;
    }

    size_t symbol = 0;
    const DeflateClass *distance_class = nullptr;
    if (!distance_symbol_for_cost_state(
            cost_model,
            repeat_distances,
            distance,
            &symbol,
            &distance_class) ||
        symbol >= cost_model->distance_symbol_count) {
        return false;
    }

    double symbol_cost = 0.0;
    if (!lookup_symbol_cost(
            cost_model->distance_symbol_costs,
            cost_model->distance_context_count,
            cost_model->distance_symbol_count,
            context,
            symbol,
            &symbol_cost)) {
        return false;
    }

    double extra_cost = 0.0;
    if (distance_class != nullptr) {
        extra_cost = static_cast<double>(distance_class->extra_bits);
    } else if (cost_model->config.distance_coding == LZSS_DISTANCE_BIT_TREE &&
               symbol == REP_DISTANCE_COUNT &&
               !distance_bit_tree_cost(cost_model, distance, &extra_cost)) {
        return false;
    }

    *out_symbol = symbol;
    *out_cost = symbol_cost + extra_cost;
    return std::isfinite(*out_cost);
}

void lzss_adaptive_ac_cost_state_init(
    LzssAdaptiveAcCostState *state)
{
    if (state == nullptr) {
        return;
    }

    repeat_distance_array_init(state->repeat_distances);
    state->literal_length_base_context = 0;
    state->literal_context = 0;
    state->length_context = 0;
    state->distance_context = 0;
}

bool lzss_adaptive_ac_literal_length_cost(
    const LzssAdaptiveAcCostModel *cost_model,
    size_t literal_length_base_context,
    size_t literal_length,
    double *out_cost)
{
    if (cost_model == nullptr ||
        out_cost == nullptr ||
        literal_length_base_context >=
            cost_model->literal_length_context_count) {
        return false;
    }

    size_t symbol = 0;
    const DeflateClass *literal_length_class = nullptr;
    if (!literal_length_symbol_for_value(
            literal_length,
            &symbol,
            &literal_length_class) ||
        symbol >= cost_model->literal_length_symbol_count) {
        return false;
    }

    double symbol_cost = 0.0;
    if (!lookup_symbol_cost(
            cost_model->literal_length_symbol_costs,
            cost_model->literal_length_context_count,
            cost_model->literal_length_symbol_count,
            literal_length_base_context,
            symbol,
            &symbol_cost)) {
        return false;
    }

    *out_cost =
        symbol_cost + static_cast<double>(literal_length_class->extra_bits);
    return std::isfinite(*out_cost);
}

bool lzss_adaptive_ac_literal_transition_cost(
    const LzssAdaptiveAcCostModel *cost_model,
    const LzssAdaptiveAcCostState *state,
    size_t literal_run_length,
    uint8_t literal,
    double *out_delta_cost,
    LzssAdaptiveAcCostState *out_state)
{
    if (cost_model == nullptr ||
        state == nullptr ||
        out_delta_cost == nullptr ||
        out_state == nullptr ||
        state->literal_context >= cost_model->literal_context_count ||
        literal_run_length == std::numeric_limits<size_t>::max()) {
        return false;
    }

    double old_length_cost = 0.0;
    double new_length_cost = 0.0;
    double literal_cost = 0.0;

    if (!lzss_adaptive_ac_literal_length_cost(
            cost_model,
            state->literal_length_base_context,
            literal_run_length,
            &old_length_cost) ||
        !lzss_adaptive_ac_literal_length_cost(
            cost_model,
            state->literal_length_base_context,
            literal_run_length + 1,
            &new_length_cost) ||
        !lookup_symbol_cost(
            cost_model->literal_costs,
            cost_model->literal_context_count,
            256,
            state->literal_context,
            literal,
            &literal_cost)) {
        return false;
    }

    *out_state = *state;
    out_state->literal_context = static_cast<size_t>(literal) + 1;
    *out_delta_cost = literal_cost + new_length_cost - old_length_cost;
    return std::isfinite(*out_delta_cost);
}

bool lzss_adaptive_ac_match_transition_cost(
    const LzssAdaptiveAcCostModel *cost_model,
    const LzssAdaptiveAcCostState *state,
    size_t literal_run_length,
    uint32_t match_length,
    uint32_t match_distance,
    double *out_delta_cost,
    LzssAdaptiveAcCostState *out_state)
{
    if (cost_model == nullptr ||
        state == nullptr ||
        out_delta_cost == nullptr ||
        out_state == nullptr ||
        match_length < cost_model->config.min_match_length ||
        match_length > cost_model->config.max_match_length ||
        match_distance == 0 ||
        match_distance > cost_model->config.window_size) {
        return false;
    }

    size_t next_literal_length_base_context = 0;
    if (!literal_length_context_after(
            cost_model,
            state->literal_length_base_context,
            literal_run_length,
            &next_literal_length_base_context)) {
        return false;
    }

    size_t length_symbol = 0;
    double length_cost = 0.0;
    if (!length_cost_for_value(
            cost_model,
            state->length_context,
            match_length,
            &length_symbol,
            &length_cost)) {
        return false;
    }

    size_t distance_symbol = 0;
    double distance_cost = 0.0;
    if (!distance_cost_for_value(
            cost_model,
            state->repeat_distances,
            state->distance_context,
            match_distance,
            &distance_symbol,
            &distance_cost)) {
        return false;
    }

    double empty_literal_length_cost = 0.0;
    if (!lzss_adaptive_ac_literal_length_cost(
            cost_model,
            next_literal_length_base_context,
            0,
            &empty_literal_length_cost)) {
        return false;
    }

    *out_state = *state;
    out_state->literal_length_base_context =
        next_literal_length_base_context;
    out_state->length_context = length_symbol + 1;
    out_state->distance_context = distance_symbol + 1;
    update_repeat_distance_array(
        out_state->repeat_distances,
        match_distance
    );

    *out_delta_cost =
        length_cost + distance_cost + empty_literal_length_cost;
    return std::isfinite(*out_delta_cost);
}

bool lzss_adaptive_ac_cost_model_init(
    const LzssConfig *config,
    const LzssSequenceStream *seed_stream,
    LzssAdaptiveAcCostModel *cost_model)
{
    if (cost_model == nullptr ||
        seed_stream == nullptr ||
        !is_valid_config(config)) {
        return false;
    }

    LzssAdaptiveAcCodec layout_codec{};
    layout_codec.config = *config;
    if (!sequence_layout_is_valid(&layout_codec, seed_stream)) {
        return false;
    }

    const size_t literal_length_symbol_count =
        active_class_count(
            LITERAL_LENGTH_CLASSES,
            array_count(LITERAL_LENGTH_CLASSES),
            0,
            UINT32_MAX
        );
    const size_t length_symbol_count =
        active_class_count(
            LENGTH_CLASSES,
            array_count(LENGTH_CLASSES),
            static_cast<uint32_t>(config->min_match_length),
            static_cast<uint32_t>(config->max_match_length)
        );
    const size_t distance_symbol_model_count =
        distance_mode_symbol_count(config);

    const size_t literal_length_context_count =
        literal_length_symbol_count + 1;
    const size_t literal_context_count = 257;
    const size_t length_context_count = length_symbol_count + 1;
    const size_t distance_context_count = distance_symbol_model_count + 1;
    const size_t distance_bit_count =
        config->distance_coding == LZSS_DISTANCE_BIT_TREE
            ? distance_bit_count_for_window(config->window_size)
            : 0;

    if (!flat_model_shape_is_valid(
            literal_length_context_count,
            literal_length_symbol_count) ||
        !flat_model_shape_is_valid(literal_context_count, 256) ||
        !flat_model_shape_is_valid(
            length_context_count,
            length_symbol_count) ||
        !flat_model_shape_is_valid(
            distance_context_count,
            distance_symbol_model_count)) {
        return false;
    }

    std::vector<uint32_t> literal_length_frequencies(
        literal_length_context_count * literal_length_symbol_count,
        1
    );
    std::vector<uint32_t> literal_frequencies(
        literal_context_count * 256,
        1
    );
    std::vector<uint32_t> length_frequencies(
        length_context_count * length_symbol_count,
        1
    );
    std::vector<uint32_t> distance_symbol_frequencies(
        distance_context_count * distance_symbol_model_count,
        1
    );

    std::vector<size_t> literal_length_totals(
        literal_length_context_count,
        literal_length_symbol_count
    );
    std::vector<size_t> literal_totals(literal_context_count, 256);
    std::vector<size_t> length_totals(
        length_context_count,
        length_symbol_count
    );
    std::vector<size_t> distance_symbol_totals(
        distance_context_count,
        distance_symbol_model_count
    );

    const size_t distance_node_count =
        distance_bit_tree_node_count(distance_bit_count);
    std::vector<uint32_t> distance_bit_frequencies(
        distance_node_count * 2,
        1
    );
    std::vector<size_t> distance_bit_totals(distance_node_count, 2);

    size_t literal_length_context = 0;
    size_t literal_context = 0;
    size_t length_context = 0;
    size_t distance_context = 0;
    uint32_t repeat_distances[REP_DISTANCE_COUNT];
    repeat_distance_array_init(repeat_distances);

    for (size_t i = 0; i < seed_stream->sequences.size(); ++i) {
        const LzssSequence& sequence = seed_stream->sequences[i];

        if (i > 0) {
            size_t length_symbol = 0;
            const DeflateClass *length_class = nullptr;
            if (!find_class_for_value(
                    LENGTH_CLASSES,
                    array_count(LENGTH_CLASSES),
                    static_cast<uint32_t>(config->min_match_length),
                    static_cast<uint32_t>(config->max_match_length),
                    sequence.match_length,
                    &length_symbol,
                    &length_class) ||
                !update_frequency(
                    &length_frequencies,
                    &length_totals,
                    length_symbol_count,
                    length_context,
                    length_symbol)) {
                return false;
            }

            (void)length_class;
            length_context = length_symbol + 1;

            LzssAdaptiveAcCostModel partial_model{};
            partial_model.config = *config;
            partial_model.distance_symbol_count =
                distance_symbol_model_count;
            partial_model.distance_bit_count = distance_bit_count;

            size_t distance_symbol = 0;
            const DeflateClass *distance_class = nullptr;
            if (!distance_symbol_for_cost_state(
                    &partial_model,
                    repeat_distances,
                    sequence.match_distance,
                    &distance_symbol,
                    &distance_class) ||
                !update_frequency(
                    &distance_symbol_frequencies,
                    &distance_symbol_totals,
                    distance_symbol_model_count,
                    distance_context,
                    distance_symbol)) {
                return false;
            }

            if (config->distance_coding == LZSS_DISTANCE_BIT_TREE &&
                distance_symbol == REP_DISTANCE_COUNT &&
                !update_distance_bit_tree_frequencies(
                    &partial_model,
                    sequence.match_distance,
                    &distance_bit_frequencies,
                    &distance_bit_totals)) {
                return false;
            }

            (void)distance_class;
            distance_context = distance_symbol + 1;
            update_repeat_distance_array(
                repeat_distances,
                sequence.match_distance
            );
        }

        size_t literal_length_symbol = 0;
        const DeflateClass *literal_length_class = nullptr;
        if (!literal_length_symbol_for_value(
                sequence.lit_length,
                &literal_length_symbol,
                &literal_length_class) ||
            !update_frequency(
                &literal_length_frequencies,
                &literal_length_totals,
                literal_length_symbol_count,
                literal_length_context,
                literal_length_symbol)) {
            return false;
        }

        (void)literal_length_class;
        literal_length_context = literal_length_symbol + 1;

        for (uint32_t literal_index = 0;
             literal_index < sequence.lit_length;
             ++literal_index) {
            const uint8_t literal = sequence.literals_ptr[literal_index];
            if (!update_frequency(
                    &literal_frequencies,
                    &literal_totals,
                    256,
                    literal_context,
                    literal)) {
                return false;
            }

            literal_context = static_cast<size_t>(literal) + 1;
        }
    }

    LzssAdaptiveAcCostModel working{};
    working.config = *config;
    working.literal_length_symbol_count = literal_length_symbol_count;
    working.length_symbol_count = length_symbol_count;
    working.distance_symbol_count = distance_symbol_model_count;
    working.literal_length_context_count = literal_length_context_count;
    working.literal_context_count = literal_context_count;
    working.length_context_count = length_context_count;
    working.distance_context_count = distance_context_count;
    working.distance_bit_count = distance_bit_count;

    if (!costs_from_frequencies(
            literal_length_frequencies,
            literal_length_totals,
            literal_length_symbol_count,
            &working.literal_length_symbol_costs) ||
        !costs_from_frequencies(
            literal_frequencies,
            literal_totals,
            256,
            &working.literal_costs) ||
        !costs_from_frequencies(
            length_frequencies,
            length_totals,
            length_symbol_count,
            &working.length_symbol_costs) ||
        !costs_from_frequencies(
            distance_symbol_frequencies,
            distance_symbol_totals,
            distance_symbol_model_count,
            &working.distance_symbol_costs)) {
        return false;
    }

    if (distance_node_count > 0 &&
        !costs_from_frequencies(
            distance_bit_frequencies,
            distance_bit_totals,
            2,
            &working.distance_bit_costs)) {
        return false;
    }

    *cost_model = std::move(working);
    return true;
}

bool lzss_adaptive_ac_codec_init(
    LzssAdaptiveAcCodec *codec,
    const LzssConfig *config)
{
    if (codec == nullptr || !is_valid_config(config)) {
        return false;
    }

    std::memset(codec, 0, sizeof(*codec));
    codec->config = *config;

    const size_t literal_length_class_count =
        active_class_count(
            LITERAL_LENGTH_CLASSES,
            array_count(LITERAL_LENGTH_CLASSES),
            0,
            UINT32_MAX
        );

    const size_t length_class_count =
        active_class_count(
            LENGTH_CLASSES,
            array_count(LENGTH_CLASSES),
            static_cast<uint32_t>(config->min_match_length),
            static_cast<uint32_t>(config->max_match_length)
        );

    const size_t distance_model_symbol_count =
        distance_mode_symbol_count(config);

    codec->literal_length_context_count = literal_length_class_count + 1;
    codec->literal_context_count = 257;
    codec->length_context_count = length_class_count + 1;
    codec->distance_context_count = distance_model_symbol_count + 1;
    codec->distance_bit_count =
        config->distance_coding == LZSS_DISTANCE_BIT_TREE
            ? distance_bit_count_for_window(config->window_size)
            : 0;

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
        config->distance_coding == LZSS_DISTANCE_BIT_TREE
            ? 0
            : max_extra_bit_count(
                  DISTANCE_CLASSES,
                  array_count(DISTANCE_CLASSES),
                  1,
                  static_cast<uint32_t>(config->window_size)
              );

    if (!create_context_models(
            &codec->literal_length_models,
            codec->literal_length_context_count,
            literal_length_class_count) ||
        !create_context_models(
            &codec->literal_models,
            codec->literal_context_count,
            256) ||
        !create_context_models(
            &codec->length_models,
            codec->length_context_count,
            length_class_count) ||
        !create_context_models(
            &codec->distance_models,
            codec->distance_context_count,
            distance_model_symbol_count) ||
        !create_bit_models(
            &codec->literal_length_extra_bit_models,
            codec->literal_length_extra_bit_count) ||
        !create_bit_models(
            &codec->length_extra_bit_models,
            codec->length_extra_bit_count) ||
        !create_bit_models(
            &codec->distance_extra_bit_models,
            codec->distance_extra_bit_count)) {
        lzss_adaptive_ac_codec_destroy(codec);
        return false;
    }

    if (config->distance_coding == LZSS_DISTANCE_BIT_TREE &&
        !create_distance_bit_tree_models(codec)) {
        lzss_adaptive_ac_codec_destroy(codec);
        return false;
    }

    return true;
}

void lzss_adaptive_ac_codec_destroy(
    LzssAdaptiveAcCodec *codec)
{
    if (codec == nullptr) {
        return;
    }

    destroy_context_models(
        codec->literal_length_models,
        codec->literal_length_context_count
    );
    destroy_context_models(
        codec->literal_models,
        codec->literal_context_count
    );
    destroy_context_models(
        codec->length_models,
        codec->length_context_count
    );
    destroy_context_models(
        codec->distance_models,
        codec->distance_context_count
    );
    destroy_distance_bit_tree_models(codec);

    destroy_bit_models(
        codec->literal_length_extra_bit_models,
        codec->literal_length_extra_bit_count
    );
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

bool lzss_adaptive_ac_encode_stream(
    LzssAdaptiveAcCodec *codec,
    struct ac *ac,
    struct bio *bio,
    const LzssSequenceStream *stream)
{
    if (codec == nullptr ||
        ac == nullptr ||
        bio == nullptr ||
        stream == nullptr ||
        !sequence_layout_is_valid(codec, stream)) {
        return false;
    }

    uint32_t sequence_count_u32 = 0;
    if (!checked_size_to_u32(stream->sequences.size(), &sequence_count_u32) ||
        !write_u32(bio, sequence_count_u32)) {
        return false;
    }

    ac_init(ac);
    RepeatDistanceState repeat_state{};
    repeat_distance_state_init(&repeat_state);
    Order1ContextState context_state{};
    order1_context_state_init(&context_state);

    for (size_t i = 0; i < stream->sequences.size(); ++i) {
        const LzssSequence *sequence = &stream->sequences[i];

        if (i > 0 &&
            !encode_match(
                codec,
                ac,
                bio,
                &repeat_state,
                &context_state,
                sequence)) {
            return false;
        }

        if (!encode_literal_length(
                codec,
                ac,
                bio,
                &context_state,
                sequence->lit_length)) {
            return false;
        }

        for (uint32_t literal_index = 0;
             literal_index < sequence->lit_length;
             ++literal_index) {
            if (!encode_symbol_order1(
                    ac,
                    bio,
                    sequence->literals_ptr[literal_index],
                    codec->literal_models,
                    codec->literal_context_count,
                    &context_state.literal_context)) {
                return false;
            }
        }
    }

    ac_encode_flush(ac, bio);
    return true;
}

bool lzss_adaptive_ac_decode_stream(
    LzssAdaptiveAcCodec *codec,
    struct ac *ac,
    struct bio *bio,
    LzssSequenceStream *out_stream)
{
    if (codec == nullptr ||
        ac == nullptr ||
        bio == nullptr ||
        out_stream == nullptr) {
        return false;
    }

    const uint32_t sequence_count = read_u32(bio);

    ac_init(ac);
    ac_decode_init(ac, bio);

    out_stream->sequences.clear();
    out_stream->literals.clear();
    out_stream->sequences.reserve(sequence_count);

    std::vector<size_t> literal_offsets;
    literal_offsets.reserve(sequence_count);

    RepeatDistanceState repeat_state{};
    repeat_distance_state_init(&repeat_state);
    Order1ContextState context_state{};
    order1_context_state_init(&context_state);

    for (uint32_t i = 0; i < sequence_count; ++i) {
        LzssSequence sequence{};

        if (i > 0 &&
            !decode_match(
                codec,
                ac,
                bio,
                &repeat_state,
                &context_state,
                &sequence)) {
            return false;
        }

        if (!decode_literal_length(
                codec,
                ac,
                bio,
                &context_state,
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
            size_t literal = 0;
            if (!decode_symbol_order1(
                    ac,
                    bio,
                    codec->literal_models,
                    codec->literal_context_count,
                    &context_state.literal_context,
                    &literal) ||
                literal > UINT8_MAX) {
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

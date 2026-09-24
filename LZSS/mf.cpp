#include "mf.h"
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

//TODO compare hash search with tree

struct LzssMatchFinder {
    size_t window_size;
    size_t hash_bytes;
    size_t hash_size;
    size_t max_chain_length;
    size_t good_match_length;
    size_t optimal_max_chain_length;
    uint32_t key_mask;
    std::vector<size_t> head;
    std::vector<size_t> next;
    std::vector<size_t> slot_position;
};

static constexpr size_t NO_POSITION = std::numeric_limits<size_t>::max();
static constexpr size_t HASH_LOAD_BYTES = sizeof(uint32_t);
static constexpr uint32_t HASH3_KEY_MASK = 0x00ffffffu;
static constexpr uint32_t HASH4_KEY_MASK = 0xffffffffu;

static size_t window_slot(const LzssMatchFinder* mf, size_t position)
{
    return position % mf->window_size;
}

static size_t get_chain_next(const LzssMatchFinder* mf, size_t position)
{
    if (mf->window_size == 0) {
        return NO_POSITION;
    }

    const size_t slot = window_slot(mf, position);
    if (mf->slot_position[slot] != position) {
        return NO_POSITION;
    }

    return mf->next[slot];
}

static uint32_t load32(const uint8_t *buffer, size_t pos)
{
    uint32_t value;
    std::memcpy(&value, buffer + pos, sizeof(value));
    return value;
}

static uint64_t load64(const uint8_t *buffer, size_t pos)
{
    uint64_t value;
    std::memcpy(&value, buffer + pos, sizeof(value));
    return value;
}

static uint32_t load_match_key(
    const LzssMatchFinder *mf,
    const uint8_t *buffer,
    size_t pos)
{
    return load32(buffer, pos) & mf->key_mask;
}

static bool is_power_of_two(size_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

static size_t hash_key(const LzssMatchFinder *mf, uint32_t value)
{
    value ^= value >> 9;
    value *= 0x9e3779b1u;
    value ^= value >> 16;
    return value & (mf->hash_size - 1);
}

static size_t count_match_length(const uint8_t *buffer,
                                 size_t left,
                                 size_t right,
                                 size_t start_len,
                                 size_t max_len)
{
    size_t len = start_len;

    while (len + sizeof(uint64_t) <= max_len &&
           load64(buffer, left + len) == load64(buffer, right + len)) {
        len += sizeof(uint64_t);
    }

    while (len < max_len && buffer[left + len] == buffer[right + len]) {
        ++len;
    }

    return len;
}

static LzssMatchFinder* match_finder_create_with_options(
    size_t window_size,
    LzssHashMode hash_mode,
    size_t hash_size,
    size_t max_chain_length,
    size_t good_match_length,
    size_t optimal_max_chain_length)
{
    if (window_size == 0 ||
        hash_size == 0 ||
        !is_power_of_two(hash_size) ||
        max_chain_length == 0 ||
        good_match_length == 0 ||
        optimal_max_chain_length == 0) {
        return nullptr;
    }

    auto* mf = new LzssMatchFinder;
    if (mf != nullptr) {
        mf->window_size = window_size;
        mf->hash_bytes = hash_mode == LZSS_HASH3 ? 3 : 4;
        mf->hash_size = hash_size;
        mf->max_chain_length = max_chain_length;
        mf->good_match_length = good_match_length;
        mf->optimal_max_chain_length = optimal_max_chain_length;
        mf->key_mask =
            hash_mode == LZSS_HASH3 ? HASH3_KEY_MASK : HASH4_KEY_MASK;
        mf->head.assign(hash_size, NO_POSITION);
        mf->next.assign(window_size, NO_POSITION);
        mf->slot_position.assign(window_size, NO_POSITION);
    }
    return mf;
}

LzssMatchFinder* match_finder_create(size_t window_size,
                                     LzssHashMode hash_mode) {
    return match_finder_create_with_options(
        window_size,
        hash_mode,
        LZSS_DEFAULT_HASH_SIZE,
        LZSS_DEFAULT_MAX_CHAIN_LENGTH,
        LZSS_DEFAULT_GOOD_MATCH_LENGTH,
        LZSS_DEFAULT_OPTIMAL_MAX_CHAIN_LENGTH
    );
}

LzssMatchFinder* match_finder_create_for_config(const LzssConfig *config)
{
    if (config == nullptr) {
        return nullptr;
    }

    return match_finder_create_with_options(
        config->window_size,
        config->hash_mode,
        config->hash_size == 0
            ? LZSS_DEFAULT_HASH_SIZE
            : config->hash_size,
        config->max_chain_length == 0
            ? LZSS_DEFAULT_MAX_CHAIN_LENGTH
            : config->max_chain_length,
        config->good_match_length == 0
            ? LZSS_DEFAULT_GOOD_MATCH_LENGTH
            : config->good_match_length,
        config->optimal_max_chain_length == 0
            ? LZSS_DEFAULT_OPTIMAL_MAX_CHAIN_LENGTH
            : config->optimal_max_chain_length
    );
}

void match_finder_destroy(LzssMatchFinder* mf) {
    delete mf;
}

void match_finder_insert_position(LzssMatchFinder *mf, const uint8_t *input,
                                  size_t position, size_t buffer_size) {
    if (mf == nullptr || input == nullptr ||
        position + HASH_LOAD_BYTES > buffer_size) {
        return;
    }

    const size_t hash = hash_key(mf, load_match_key(mf, input, position));
    const size_t slot = window_slot(mf, position);

    mf->next[slot] = mf->head[hash];
    mf->slot_position[slot] = position;
    mf->head[hash] = position;
}

bool match_finder_get_best(LzssMatchFinder* mf, const uint8_t* buffer,
                           size_t pos, size_t buffer_size,
                           size_t min_len, size_t max_len,
                           LzssMatch* out_match) {
    if (!mf || !buffer || !out_match) return false;

    if (min_len == 0 || pos + min_len > buffer_size) return false;

    size_t best_len = 0;
    size_t best_dist = 0;

    size_t max_possible_len = buffer_size - pos;
    if (max_len > max_possible_len) max_len = max_possible_len;

    if (max_len < min_len) return false;

    if (pos + HASH_LOAD_BYTES > buffer_size || min_len < mf->hash_bytes) {
        return false;
    }

    const uint32_t current_key = load_match_key(mf, buffer, pos);
    const size_t hash = hash_key(mf, current_key);
    size_t search_pos = mf->head[hash];
    size_t chain_len = 0;

    while (search_pos != NO_POSITION &&
           chain_len < mf->max_chain_length) {
        ++chain_len;

        if (search_pos >= pos) {
            search_pos = get_chain_next(mf, search_pos);
            continue;
        }

        const size_t distance = pos - search_pos;
        if (distance > mf->window_size) {
            break;
        }

        if (load_match_key(mf, buffer, search_pos) != current_key) {
            search_pos = get_chain_next(mf, search_pos);
            continue;
        }

        if (best_len > 0 &&
            best_len < max_len &&
            buffer[search_pos + best_len] != buffer[pos + best_len]) {
            search_pos = get_chain_next(mf, search_pos);
            continue;
        }

        const size_t current_len =
            count_match_length(
                buffer,
                search_pos,
                pos,
                mf->hash_bytes,
                max_len
            );

        if (current_len >= min_len && current_len > best_len) {
            best_len = current_len;
            best_dist = distance;

            const size_t good_match_length =
                std::min(mf->good_match_length, max_len);
            if (best_len >= good_match_length) break;
        }

        search_pos = get_chain_next(mf, search_pos);
    }

    if (best_len >= min_len) {
        out_match->length = (uint32_t)best_len;
        out_match->distance = (uint32_t)best_dist;
        return true;
    }

    return false;
}

bool match_finder_get_matches(LzssMatchFinder* mf, const uint8_t* buffer,
                              size_t pos, size_t buffer_size,
                              size_t min_len, size_t max_len,
                              std::vector<LzssMatch>* out_matches) {
    if (!mf || !buffer || !out_matches) return false;

    out_matches->clear();

    if (min_len == 0 || pos + min_len > buffer_size) return true;

    size_t max_possible_len = buffer_size - pos;
    if (max_len > max_possible_len) max_len = max_possible_len;

    if (max_len < min_len) return true;

    if (pos + HASH_LOAD_BYTES > buffer_size || min_len < mf->hash_bytes) {
        return true;
    }

    const uint32_t current_key = load_match_key(mf, buffer, pos);
    const size_t hash = hash_key(mf, current_key);
    size_t search_pos = mf->head[hash];
    std::vector<size_t> best_distances(max_len + 1, 0);
    size_t remaining_lengths = max_len - min_len + 1;
    size_t chain_len = 0;

    while (search_pos != NO_POSITION &&
           remaining_lengths > 0 &&
           chain_len < mf->optimal_max_chain_length) {
        ++chain_len;

        if (search_pos >= pos) {
            search_pos = get_chain_next(mf, search_pos);
            continue;
        }

        const size_t distance = pos - search_pos;
        if (distance > mf->window_size) {
            break;
        }

        if (load_match_key(mf, buffer, search_pos) != current_key) {
            search_pos = get_chain_next(mf, search_pos);
            continue;
        }

        const size_t current_len =
            count_match_length(
                buffer,
                search_pos,
                pos,
                mf->hash_bytes,
                max_len
        );

        if (current_len >= min_len) {
            for (size_t length = min_len; length <= current_len; ++length) {
                if (best_distances[length] == 0) {
                    best_distances[length] = distance;
                    --remaining_lengths;
                }
            }
        }

        search_pos = get_chain_next(mf, search_pos);
    }

    out_matches->reserve(max_len - min_len + 1);
    for (size_t length = min_len; length <= max_len; ++length) {
        if (best_distances[length] == 0) {
            continue;
        }

        LzssMatch match{};
        match.length = static_cast<uint32_t>(length);
        match.distance = static_cast<uint32_t>(best_distances[length]);
        out_matches->push_back(match);
    }

    return true;
}

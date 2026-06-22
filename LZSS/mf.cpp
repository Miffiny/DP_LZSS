#include "mf.h"
#include <cstdlib>
#include <algorithm>
#include <limits>
#include <vector>

//TODO compare hash search with tree

struct LzssMatchFinder {
    size_t window_size;
    std::vector<size_t> head;
    std::vector<size_t> next;
};

static constexpr size_t NO_POSITION = std::numeric_limits<size_t>::max();
static constexpr size_t HASH_SIZE = 1u << 16;
static constexpr size_t MAX_CHAIN_LENGTH = 256;

static size_t hash3(const uint8_t *buffer, size_t pos)
{
    uint32_t value =
        (static_cast<uint32_t>(buffer[pos]) << 16) ^
        (static_cast<uint32_t>(buffer[pos + 1]) << 8) ^
        static_cast<uint32_t>(buffer[pos + 2]);

    value ^= value >> 9;
    value *= 0x9e3779b1u;
    value ^= value >> 16;
    return value & (HASH_SIZE - 1);
}

LzssMatchFinder* match_finder_create(size_t window_size) {
    auto* mf = new LzssMatchFinder;
    if (mf != nullptr) {
        mf->window_size = window_size;
        mf->head.assign(HASH_SIZE, NO_POSITION);
    }
    return mf;
}

void match_finder_destroy(LzssMatchFinder* mf) {
    delete mf;
}

void match_finder_insert_position(LzssMatchFinder *mf, const uint8_t *input,
                                  size_t position, size_t buffer_size) {
    if (mf == nullptr || input == nullptr || position + 3 > buffer_size) {
        return;
    }

    if (mf->next.size() <= position) {
        mf->next.resize(position + 1, NO_POSITION);
    }

    const size_t hash = hash3(input, position);
    mf->next[position] = mf->head[hash];
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

    if (pos + 3 > buffer_size || min_len < 3) {
        return false;
    }

    const size_t hash = hash3(buffer, pos);
    size_t search_pos = mf->head[hash];
    size_t chain_len = 0;

    while (search_pos != NO_POSITION && chain_len < MAX_CHAIN_LENGTH) {
        ++chain_len;

        if (search_pos >= pos) {
            search_pos = search_pos < mf->next.size() ? mf->next[search_pos] : NO_POSITION;
            continue;
        }

        const size_t distance = pos - search_pos;
        if (distance > mf->window_size) {
            break;
        }

        size_t current_len = 0;

        while (current_len < max_len &&
               buffer[search_pos + current_len] == buffer[pos + current_len]) {
            current_len++;
        }

        if (current_len >= min_len && current_len > best_len) {
            best_len = current_len;
            best_dist = distance;

            if (best_len == max_len) break;
        }

        search_pos = search_pos < mf->next.size() ? mf->next[search_pos] : NO_POSITION;
    }

    if (best_len >= min_len) {
        out_match->length = (uint32_t)best_len;
        out_match->distance = (uint32_t)best_dist;
        return true;
    }

    return false;
}

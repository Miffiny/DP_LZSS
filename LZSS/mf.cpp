#include "mf.h"
#include <cstdlib>

struct LzssMatchFinder {
    size_t window_size;
};

LzssMatchFinder* match_finder_create(size_t window_size) {
    auto* mf = (LzssMatchFinder*)malloc(sizeof(LzssMatchFinder));
    if (mf) {
        mf->window_size = window_size;
    }
    return mf;
}

void match_finder_destroy(LzssMatchFinder* mf) {
    free(mf);
}

void match_finder_insert_position(LzssMatchFinder *mf, const uint8_t *input, size_t position) {
    (void)mf;
    (void)input;
    (void)position;
}

bool match_finder_get_best(LzssMatchFinder* mf, const uint8_t* buffer,
                           size_t pos, size_t buffer_size,
                           size_t min_len, size_t max_len,
                           LzssMatch* out_match) {
    if (!mf || !buffer || !out_match) return false;

    size_t best_len = 0;
    size_t best_dist = 0;

    size_t window_start = (pos > mf->window_size) ? pos - mf->window_size : 0;

    size_t max_possible_len = buffer_size - pos;
    if (max_len > max_possible_len) max_len = max_possible_len;

    if (max_len < min_len) return false;

    for (size_t search_pos = pos - 1; search_pos >= window_start && search_pos < pos; --search_pos) {
        size_t current_len = 0;

        while (current_len < max_len &&
               buffer[search_pos + current_len] == buffer[pos + current_len]) {
            current_len++;
        }

        if (current_len >= min_len && current_len > best_len) {
            best_len = current_len;
            best_dist = pos - search_pos;

            if (best_len == max_len) break;
        }

        if (search_pos == 0) break;
    }

    if (best_len >= min_len) {
        out_match->length = (uint32_t)best_len;
        out_match->distance = (uint32_t)best_dist;
        return true;
    }

    return false;
}
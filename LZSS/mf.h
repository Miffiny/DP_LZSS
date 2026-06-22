#pragma once

#include <cstdint>
#include <cstddef>

typedef struct LzssMatchFinder LzssMatchFinder;

typedef struct {
    uint32_t distance;
    uint32_t length;
} LzssMatch;

LzssMatchFinder* match_finder_create(size_t window_size);
void match_finder_destroy(LzssMatchFinder* mf);
void match_finder_insert_position(LzssMatchFinder *mf, const uint8_t *input,
                                  size_t position, size_t buffer_size);
bool match_finder_get_best(LzssMatchFinder* mf, const uint8_t* buffer,
                           size_t pos, size_t buffer_size,
                           size_t min_len, size_t max_len,
                           LzssMatch* out_match);

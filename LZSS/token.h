#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

struct LzssSequence {
    uint32_t match_distance;
    uint32_t match_length;

    uint32_t lit_length;
    const uint8_t* literals_ptr;
};

struct LzssSequenceStream {
    std::vector<LzssSequence> sequences;
    std::vector<uint8_t> literals;
};

void sequence_stream_init(LzssSequenceStream* stream, size_t initial_capacity);
void sequence_stream_push(LzssSequenceStream* stream, LzssSequence seq);
void sequence_stream_free(LzssSequenceStream* stream);

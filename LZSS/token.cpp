#include "token.h"

void sequence_stream_init(LzssSequenceStream* stream, size_t initial_capacity) {
    if (!stream) return;
    stream->sequences.clear();
    stream->literals.clear();
    stream->sequences.reserve(initial_capacity > 0 ? initial_capacity : 1024);
}

void sequence_stream_push(LzssSequenceStream* stream, LzssSequence seq) {
    if (!stream) return;
    stream->sequences.push_back(seq);
}

void sequence_stream_free(LzssSequenceStream* stream) {
    if (!stream) return;
    std::vector<LzssSequence>().swap(stream->sequences);
    std::vector<uint8_t>().swap(stream->literals);
}

#include "token.h"

void token_stream_init(LzssTokenStream* stream, size_t initial_capacity) {
    if (!stream) return;
    stream->tokens.clear();
    stream->tokens.reserve(initial_capacity > 0 ? initial_capacity : 1024);
}

void token_stream_push(LzssTokenStream* stream, LzssToken token) {
    if (!stream) return;
    stream->tokens.push_back(token);
}

void token_stream_free(LzssTokenStream* stream) {
    if (!stream) return;
    std::vector<LzssToken>().swap(stream->tokens);
}

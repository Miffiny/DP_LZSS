#include "token.h"
#include <cstdlib>

void token_stream_init(LzssTokenStream* stream, size_t initial_capacity) {
    if (!stream) return;
    stream->count = 0;
    stream->capacity = initial_capacity > 0 ? initial_capacity : 1024;
    stream->tokens = (LzssToken*)malloc(stream->capacity * sizeof(LzssToken));
}

void token_stream_push(LzssTokenStream* stream, LzssToken token) {
    if (!stream) return;

    if (stream->count >= stream->capacity) {
        stream->capacity = stream->capacity == 0 ? 1024 : stream->capacity * 2;
        stream->tokens = (LzssToken*)realloc(stream->tokens, stream->capacity * sizeof(LzssToken));
    }

    stream->tokens[stream->count++] = token;
}

void token_stream_free(LzssTokenStream* stream) {
    if (!stream) return;
    free(stream->tokens);
    stream->tokens = nullptr;
    stream->count = 0;
    stream->capacity = 0;
}
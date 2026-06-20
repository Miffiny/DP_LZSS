#pragma once

#include <cstdint>
#include <cstddef>

typedef enum {
    LZSS_TOKEN_LITERAL,
    LZSS_TOKEN_MATCH,
    LZSS_TOKEN_EOF
} LzssTokenType;

typedef struct {
    LzssTokenType type;
    union {
        uint8_t literal;
        struct {
            uint32_t distance;
            uint32_t length;
        } match;
    };
} LzssToken;

//dynamic array, might be changed
typedef struct {
    LzssToken* tokens;
    size_t count;
    size_t capacity;
} LzssTokenStream;

void token_stream_init(LzssTokenStream* stream, size_t initial_capacity);
void token_stream_push(LzssTokenStream* stream, LzssToken token);
void token_stream_free(LzssTokenStream* stream);

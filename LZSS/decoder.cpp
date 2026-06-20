#include "decoder.h"
#include <cstdlib>
#include <cstring>

void buffer_init(ByteBuffer *buffer) {
    if (buffer) {
        buffer->data = nullptr;
        buffer->size = 0;
        buffer->capacity = 0;
    }
}

void buffer_init_with_capacity(ByteBuffer *buffer, size_t initial_capacity) {
    if (!buffer) return;
    buffer->data = (uint8_t*)malloc(initial_capacity);
    buffer->size = 0;
    buffer->capacity = buffer->data ? initial_capacity : 0;
}

void buffer_free(ByteBuffer *buffer) {
    if (buffer) {
        free(buffer->data);
        buffer->data = nullptr;
        buffer->size = 0;
        buffer->capacity = 0;
    }
}

static bool buffer_ensure_capacity(ByteBuffer *buffer, size_t needed) {
    if (buffer->size + needed <= buffer->capacity) return true;

    size_t new_capacity = buffer->capacity == 0 ? 1024 : buffer->capacity * 2;
    while (new_capacity < buffer->size + needed) {
        new_capacity *= 2;
    }

    auto *new_data = (uint8_t*)realloc(buffer->data, new_capacity);
    if (!new_data) return false;

    buffer->data = new_data;
    buffer->capacity = new_capacity;
    return true;
}

bool buffer_push_back(ByteBuffer *buffer, uint8_t byte) {
    if (!buffer_ensure_capacity(buffer, 1)) return false;
    buffer->data[buffer->size++] = byte;
    return true;
}

bool buffer_append(ByteBuffer *buffer, const uint8_t *src, size_t length) {
    if (!buffer_ensure_capacity(buffer, length)) return false;
    memcpy(buffer->data + buffer->size, src, length);
    buffer->size += length;
    return true;
}

bool buffer_copy_match(ByteBuffer *buffer, uint32_t distance, uint32_t length) {
    // quick checks
    if (distance == 0 || distance > buffer->size) return false;
    if (!buffer_ensure_capacity(buffer, length)) return false;

    // Find start of match
    size_t start_pos = buffer->size - distance;

    for (uint32_t i = 0; i < length; i++) {
        buffer->data[buffer->size] = buffer->data[start_pos + i];
        buffer->size++;
    }

    return true;
}

bool lzss_decode(const LzssTokenStream *in_stream, ByteBuffer *out)
{
    if (in_stream == nullptr || out == nullptr) {
        return false;
    }

    if (in_stream->count > 0 && in_stream->tokens == nullptr) {
        return false;
    }

    // The caller owns the buffer so reuse its allocated storage if available
    out->size = 0;

    bool eof_seen = false;

    for (size_t i = 0; i < in_stream->count; ++i) {
        const LzssToken &token = in_stream->tokens[i];

        switch (token.type) {
        case LZSS_TOKEN_LITERAL:
            if (eof_seen) {
                return false;
            }

            if (!buffer_push_back(out, token.literal)) {
                return false;
            }
            break;

        case LZSS_TOKEN_MATCH:
            if (eof_seen) {
                return false;
            }

            // distance is additionally checked inside buffer_copy_match
            if (token.match.length == 0) {
                return false;
            }

            if (!buffer_copy_match(
                    out,
                    token.match.distance,
                    token.match.length)) {
                return false;
                    }
            break;

        case LZSS_TOKEN_EOF:
            // EOF must occur exactly once and be the final token
            if (eof_seen || i + 1 != in_stream->count) {
                return false;
            }

            eof_seen = true;
            break;

        default:
            return false;
        }
    }

    return eof_seen;
}
#pragma once

#include <cstdint>
#include <cstddef>
#include "token.h"

typedef struct {
    uint8_t* data;      // pointer for byte array in heap
    size_t size;        // number of written bytes
    size_t capacity;    // max memory
} ByteBuffer;

// init + clear
void buffer_init(ByteBuffer *buffer);
void buffer_init_with_capacity(ByteBuffer *buffer, size_t initial_capacity);
void buffer_free(ByteBuffer *buffer);

// writing data
bool buffer_append(ByteBuffer *buffer, const uint8_t *src, size_t length);

// copy match to buffer
bool buffer_copy_match(ByteBuffer *buffer, uint32_t distance, uint32_t length);

bool lzss_decode(const LzssSequenceStream *in_stream, ByteBuffer *out);

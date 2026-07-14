#ifndef TANS_H
#define TANS_H

#include <stdint.h>
#include <stddef.h>
#include "bio.h"
#include "ac.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TANS_L 4096

struct tans_decode_entry {
    uint16_t new_x;
    uint16_t symbol;
    uint8_t num_bits;
};

struct tans_model {
    struct tans_decode_entry decode_table[TANS_L];

    uint16_t* symbol_cum;
    uint16_t* symbol_freqs;
    uint32_t count;
};

struct tans_bit_chunk {
    uint32_t bits;
    uint8_t count;
};

struct tans_state {
    uint32_t x;
    struct tans_bit_chunk *chunks;
    size_t chunk_count;
    size_t chunk_capacity;
};

int tans_model_init(struct tans_model *tm, const struct model *m);
void tans_model_destroy(struct tans_model *tm);

void tans_encode_init(struct tans_state *ts);
void tans_encode_symbol(struct tans_state *ts, struct bio *bio, size_t symb, const struct tans_model *tm);
void tans_encode_flush(struct tans_state *ts, struct bio *bio);

void tans_decode_init(struct tans_state *ts, struct bio *bio);
size_t tans_decode_symbol(struct tans_state *ts, struct bio *bio, const struct tans_model *tm);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

#include "parser.h"
#include "token.h"

struct LzssTansCodec {
    LzssConfig config;

    size_t length_extra_bit_count;
    size_t distance_extra_bit_count;

    struct model event_model;
    struct model literal_model;
    struct model length_model;
    struct model distance_model;

    struct tans_model event_tans_model;
    struct tans_model literal_tans_model;
    struct tans_model length_tans_model;
    struct tans_model distance_tans_model;

    bool event_tans_ready;
    bool literal_tans_ready;
    bool length_tans_ready;
    bool distance_tans_ready;
};

bool lzss_tans_codec_init(
    LzssTansCodec *codec,
    const LzssConfig *config
);

void lzss_tans_codec_destroy(
    LzssTansCodec *codec
);

bool lzss_tans_encode_stream(
    LzssTansCodec *codec,
    struct bio *bio,
    const LzssTokenStream *stream
);

bool lzss_tans_decode_stream(
    LzssTansCodec *codec,
    struct bio *bio,
    LzssTokenStream *out_stream
);

#endif

#endif

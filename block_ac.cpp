#include "block_ac.h"

#include "LZSS/optimal_parser.h"
#include "adaptive_ac.h"
#include "tans.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

struct AcCodecGuard {
    LzssAdaptiveAcCodec codec{};
    bool initialized = false;

    bool init(const LzssConfig *config)
    {
        initialized = lzss_adaptive_ac_codec_init(&codec, config);
        return initialized;
    }

    ~AcCodecGuard()
    {
        if (initialized) {
            lzss_adaptive_ac_codec_destroy(&codec);
        }
    }

    AcCodecGuard() = default;
    AcCodecGuard(const AcCodecGuard&) = delete;
    AcCodecGuard& operator=(const AcCodecGuard&) = delete;
};

struct TansCodecGuardForCost {
    LzssTansCodec codec{};
    bool initialized = false;

    bool init(const LzssConfig *config)
    {
        initialized = lzss_tans_codec_init(&codec, config);
        return initialized;
    }

    ~TansCodecGuardForCost()
    {
        if (initialized) {
            lzss_tans_codec_destroy(&codec);
        }
    }

    TansCodecGuardForCost() = default;
    TansCodecGuardForCost(const TansCodecGuardForCost&) = delete;
    TansCodecGuardForCost& operator=(const TansCodecGuardForCost&) = delete;
};

struct EncodedAcBlockResult {
    std::vector<uint32_t> compressed_words;
    LzssBlockStats stats{};
};

static size_t block_count_for_size(size_t input_size, size_t block_size)
{
    if (input_size == 0) {
        return 1;
    }

    return (input_size - 1) / block_size + 1;
}

static size_t block_uncompressed_size(
    size_t original_size,
    size_t block_size,
    size_t block_count,
    size_t block_index)
{
    if (original_size == 0) {
        return 0;
    }

    if (block_index + 1 == block_count) {
        return original_size - block_size * (block_count - 1);
    }

    return block_size;
}

static size_t worker_count_for(size_t block_count, size_t max_workers)
{
    if (block_count == 0 || max_workers == 0) {
        return 0;
    }

    const unsigned int hardware_threads = std::thread::hardware_concurrency();
    const size_t hardware_count =
        hardware_threads == 0 ? 1 : static_cast<size_t>(hardware_threads);

    return std::min({block_count, hardware_count, max_workers});
}

static void collect_sequence_stats(
    const LzssSequenceStream& stream,
    LzssBlockStats *stats)
{
    stats->token_count = stream.sequences.size();

    for (const LzssSequence& sequence : stream.sequences) {
        stats->literal_token_count += sequence.lit_length;

        if (sequence.match_length > 0) {
            stats->match_token_count++;
            stats->match_memory +=
                sizeof(sequence.match_distance) +
                sizeof(sequence.match_length);
            stats->match_length_total += sequence.match_length;
        }
    }
}

static void add_stats(LzssBlockStats *dst, const LzssBlockStats& src)
{
    dst->token_count += src.token_count;
    dst->match_token_count += src.match_token_count;
    dst->literal_token_count += src.literal_token_count;
    dst->match_memory += src.match_memory;
    dst->match_length_total += src.match_length_total;
}

static bool ensure_buffer_capacity(ByteBuffer *buffer, size_t needed)
{
    if (needed <= buffer->capacity) {
        return true;
    }

    void *new_data = std::realloc(buffer->data, needed);
    if (new_data == nullptr && needed != 0) {
        return false;
    }

    buffer->data = static_cast<uint8_t *>(new_data);
    buffer->capacity = needed;
    return true;
}

static bool build_sequence_stream_for_block(
    const uint8_t *block_input,
    size_t current_block_size,
    const LzssConfig *config,
    LzssSequenceStream *sequence_stream)
{
    if (config->parse_mode != LZSS_PARSE_OPTIMAL) {
        return lzss_encode(
            block_input,
            current_block_size,
            config,
            sequence_stream
        );
    }

    LzssConfig seed_config = *config;
    seed_config.parse_mode = LZSS_PARSE_LAZY;

    LzssSequenceStream seed_stream;
    sequence_stream_init(&seed_stream, 0);

    TansCodecGuardForCost cost_codec;
    LzssTansCostModel cost_model;

    const bool ok =
        lzss_encode(
            block_input,
            current_block_size,
            &seed_config,
            &seed_stream
        ) &&
        cost_codec.init(config) &&
        lzss_tans_build_models(&cost_codec.codec, &seed_stream, true) &&
        lzss_tans_cost_model_init(&cost_codec.codec, &cost_model) &&
        lzss_encode_optimal(
            block_input,
            current_block_size,
            config,
            &cost_model,
            sequence_stream
        );

    sequence_stream_free(&seed_stream);
    return ok;
}

static bool encode_one_block(
    const uint8_t *input,
    size_t input_size,
    size_t block_size,
    size_t block_count,
    size_t block_index,
    const LzssConfig *config,
    EncodedAcBlockResult *result)
{
    const size_t block_offset = block_index * block_size;
    const size_t current_block_size = block_uncompressed_size(
        input_size,
        block_size,
        block_count,
        block_index
    );
    const uint8_t *block_input =
        current_block_size == 0 ? nullptr : input + block_offset;

    LzssSequenceStream sequence_stream;
    sequence_stream_init(&sequence_stream, 0);

    AcCodecGuard codec;
    if (!codec.init(config) ||
        !build_sequence_stream_for_block(
            block_input,
            current_block_size,
            config,
            &sequence_stream)) {
        sequence_stream_free(&sequence_stream);
        return false;
    }

    collect_sequence_stats(sequence_stream, &result->stats);

    const size_t word_count = std::max<size_t>(
        1024,
        sequence_stream.sequences.size() * 16 + current_block_size * 2 + 128
    );
    std::vector<uint32_t> compressed_words(word_count, 0);

    bio writer{};
    bio_open(
        &writer,
        compressed_words.data(),
        compressed_words.data() + compressed_words.size(),
        BIO_MODE_WRITE
    );

    ac ac_state{};
    const bool encoded =
        lzss_adaptive_ac_encode_stream(
            &codec.codec,
            &ac_state,
            &writer,
            &sequence_stream
        );
    bio_close(&writer, BIO_MODE_WRITE);

    sequence_stream_free(&sequence_stream);

    if (!encoded) {
        return false;
    }

    const size_t used_words =
        static_cast<size_t>(writer.ptr - compressed_words.data());
    compressed_words.resize(used_words);
    result->compressed_words = std::move(compressed_words);
    return true;
}

static bool decode_one_block(
    const LzssAcBlockStream *stream,
    const LzssConfig *config,
    ByteBuffer *out,
    size_t block_index)
{
    const size_t block_count = stream->blocks.size();
    const size_t expected_size = block_uncompressed_size(
        stream->original_size,
        stream->block_size,
        block_count,
        block_index
    );
    const LzssAcBlock& block = stream->blocks[block_index];

    if (block.compressed_words.empty()) {
        return false;
    }

    AcCodecGuard codec;
    if (!codec.init(config)) {
        return false;
    }

    LzssSequenceStream decoded_stream;
    sequence_stream_init(&decoded_stream, 0);

    bio reader{};
    bio_open(
        &reader,
        const_cast<uint32_t *>(block.compressed_words.data()),
        const_cast<uint32_t *>(block.compressed_words.data()) +
            block.compressed_words.size(),
        BIO_MODE_READ
    );

    ac ac_state{};
    bool ok = lzss_adaptive_ac_decode_stream(
        &codec.codec,
        &ac_state,
        &reader,
        &decoded_stream
    );

    ByteBuffer decoded_bytes;
    buffer_init(&decoded_bytes);
    buffer_init_with_capacity(&decoded_bytes, expected_size);

    if (ok) {
        ok = lzss_decode(&decoded_stream, &decoded_bytes);
    }

    if (ok && decoded_bytes.size == expected_size && expected_size > 0) {
        const size_t output_offset = block_index * stream->block_size;
        std::memcpy(out->data + output_offset,
                    decoded_bytes.data,
                    expected_size);
    } else if (ok && decoded_bytes.size != expected_size) {
        ok = false;
    }

    buffer_free(&decoded_bytes);
    sequence_stream_free(&decoded_stream);
    return ok;
}

void lzss_ac_block_stream_init(LzssAcBlockStream *stream)
{
    if (stream == nullptr) {
        return;
    }

    stream->original_size = 0;
    stream->block_size = LZSS_TANS_DEFAULT_BLOCK_SIZE;
    stream->blocks.clear();
    stream->stats = {};
}

void lzss_ac_block_stream_clear(LzssAcBlockStream *stream)
{
    if (stream == nullptr) {
        return;
    }

    LzssAcBlockStream empty{};
    empty.block_size = LZSS_TANS_DEFAULT_BLOCK_SIZE;
    *stream = std::move(empty);
}

size_t lzss_ac_block_stream_compressed_size(
    const LzssAcBlockStream *stream)
{
    if (stream == nullptr) {
        return 0;
    }

    size_t compressed_size = 0;
    for (const LzssAcBlock& block : stream->blocks) {
        compressed_size += block.compressed_words.size() * sizeof(uint32_t);
    }

    return compressed_size;
}

bool lzss_ac_encode_blocks(
    const uint8_t *input,
    size_t input_size,
    size_t block_size,
    size_t max_workers,
    const LzssConfig *config,
    LzssAcBlockStream *out_stream)
{
    if ((input_size > 0 && input == nullptr) ||
        block_size == 0 ||
        max_workers == 0 ||
        config == nullptr ||
        out_stream == nullptr) {
        return false;
    }

    try {
        const size_t block_count =
            block_count_for_size(input_size, block_size);

        LzssAcBlockStream working{};
        working.original_size = input_size;
        working.block_size = block_size;
        working.blocks.resize(block_count);

        std::vector<EncodedAcBlockResult> results(block_count);
        std::atomic<size_t> next_block{0};
        std::atomic<bool> ok{true};

        const size_t worker_count = worker_count_for(block_count, max_workers);
        std::vector<std::thread> workers;
        workers.reserve(worker_count);

        for (size_t worker_index = 0; worker_index < worker_count;
             ++worker_index) {
            workers.emplace_back([&]() {
                while (ok.load(std::memory_order_relaxed)) {
                    const size_t block_index =
                        next_block.fetch_add(1, std::memory_order_relaxed);
                    if (block_index >= block_count) {
                        return;
                    }

                    try {
                        if (!encode_one_block(
                                input,
                                input_size,
                                block_size,
                                block_count,
                                block_index,
                                config,
                                &results[block_index])) {
                            ok.store(false, std::memory_order_relaxed);
                            return;
                        }
                    } catch (...) {
                        ok.store(false, std::memory_order_relaxed);
                        return;
                    }
                }
            });
        }

        for (std::thread& worker : workers) {
            worker.join();
        }

        if (!ok.load(std::memory_order_relaxed)) {
            return false;
        }

        for (size_t block_index = 0; block_index < block_count;
             ++block_index) {
            working.blocks[block_index].compressed_words =
                std::move(results[block_index].compressed_words);
            add_stats(&working.stats, results[block_index].stats);
        }

        *out_stream = std::move(working);
        return true;
    } catch (...) {
        return false;
    }
}

bool lzss_ac_decode_blocks(
    const LzssAcBlockStream *stream,
    size_t max_workers,
    const LzssConfig *config,
    ByteBuffer *out)
{
    if (stream == nullptr ||
        config == nullptr ||
        out == nullptr ||
        max_workers == 0 ||
        stream->block_size == 0) {
        return false;
    }

    try {
        const size_t expected_block_count =
            block_count_for_size(stream->original_size, stream->block_size);
        if (stream->blocks.size() != expected_block_count) {
            return false;
        }

        if (!ensure_buffer_capacity(out, stream->original_size)) {
            return false;
        }
        out->size = 0;

        std::atomic<size_t> next_block{0};
        std::atomic<bool> ok{true};

        const size_t worker_count =
            worker_count_for(stream->blocks.size(), max_workers);
        std::vector<std::thread> workers;
        workers.reserve(worker_count);

        for (size_t worker_index = 0; worker_index < worker_count;
             ++worker_index) {
            workers.emplace_back([&]() {
                while (ok.load(std::memory_order_relaxed)) {
                    const size_t block_index =
                        next_block.fetch_add(1, std::memory_order_relaxed);
                    if (block_index >= stream->blocks.size()) {
                        return;
                    }

                    try {
                        if (!decode_one_block(
                                stream,
                                config,
                                out,
                                block_index)) {
                            ok.store(false, std::memory_order_relaxed);
                            return;
                        }
                    } catch (...) {
                        ok.store(false, std::memory_order_relaxed);
                        return;
                    }
                }
            });
        }

        for (std::thread& worker : workers) {
            worker.join();
        }

        if (!ok.load(std::memory_order_relaxed)) {
            out->size = 0;
            return false;
        }

        out->size = stream->original_size;
        return true;
    } catch (...) {
        out->size = 0;
        return false;
    }
}

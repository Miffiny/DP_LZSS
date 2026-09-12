#include "block_tans.h"

#include "LZSS/optimal_parser.h"
#include "tans.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

struct TansCodecGuard {
    LzssTansCodec codec{};
    bool initialized = false;

    bool init(const LzssConfig *config)
    {
        initialized = lzss_tans_codec_init(&codec, config);
        return initialized;
    }

    ~TansCodecGuard()
    {
        if (initialized) {
            lzss_tans_codec_destroy(&codec);
        }
    }

    TansCodecGuard() = default;
    TansCodecGuard(const TansCodecGuard&) = delete;
    TansCodecGuard& operator=(const TansCodecGuard&) = delete;
};

struct EncodedBlockResult {
    std::vector<uint32_t> compressed_words;
    LzssBlockStats stats{};
    LzssPayloadStats payload_stats{};
    LzssBlockTimings timings{};
};

static double elapsed_ms_since(
    const std::chrono::steady_clock::time_point& start)
{
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start
    ).count();
}

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

static bool is_valid_table_mode(LzssTansTableMode table_mode)
{
    return table_mode == LZSS_TANS_TABLE_LAZY ||
           table_mode == LZSS_TANS_TABLE_REBUILD;
}

static void collect_sequence_stats(
    const LzssSequenceStream& stream,
    LzssBlockStats *stats)
{
    stats->token_count = stream.sequences.size();
    uint32_t repeat_distances[3] = {0, 0, 0};

    for (const LzssSequence& sequence : stream.sequences) {
        stats->literal_token_count += sequence.lit_length;

        if (sequence.match_length > 0) {
            stats->match_token_count++;
            stats->match_memory +=
                sizeof(sequence.match_distance) +
                sizeof(sequence.match_length);
            stats->match_length_total += sequence.match_length;

            size_t repeat_index = 3;
            for (size_t i = 0; i < 3; ++i) {
                if (repeat_distances[i] == sequence.match_distance) {
                    repeat_index = i;
                    break;
                }
            }

            if (repeat_index == 0) {
                stats->rep0_count++;
            } else if (repeat_index == 1) {
                stats->rep1_count++;
            } else if (repeat_index == 2) {
                stats->rep2_count++;
            } else {
                stats->new_distance_count++;
            }

            const uint32_t distance = sequence.match_distance;
            if (repeat_index < 3) {
                for (size_t i = repeat_index; i > 0; --i) {
                    repeat_distances[i] = repeat_distances[i - 1];
                }
            } else {
                repeat_distances[2] = repeat_distances[1];
                repeat_distances[1] = repeat_distances[0];
            }
            repeat_distances[0] = distance;
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
    dst->rep0_count += src.rep0_count;
    dst->rep1_count += src.rep1_count;
    dst->rep2_count += src.rep2_count;
    dst->new_distance_count += src.new_distance_count;
}

static void add_payload_stats(
    LzssPayloadStats *dst,
    const LzssPayloadStats& src)
{
    dst->exact_payload_bits += src.exact_payload_bits;
    dst->rounded_payload_bytes += src.rounded_payload_bytes;
    dst->model_header_bits += src.model_header_bits;
    dst->tans_stream_bits += src.tans_stream_bits;
    dst->extra_stream_bits += src.extra_stream_bits;
    dst->padding_bits += src.padding_bits;
    dst->total_stream_bytes += src.total_stream_bytes;
    dst->empirical_entropy_bits += src.empirical_entropy_bits;
    dst->normalization_loss_bits += src.normalization_loss_bits;
    dst->tans_coder_overhead_bits += src.tans_coder_overhead_bits;
}

static void add_timings(
    LzssBlockTimings *dst,
    const LzssBlockTimings& src)
{
    dst->parse_ms += src.parse_ms;
    dst->model_build_ms += src.model_build_ms;
    dst->entropy_encode_ms += src.entropy_encode_ms;
    dst->entropy_decode_ms += src.entropy_decode_ms;
    dst->reconstruct_ms += src.reconstruct_ms;
}

static void copy_tans_payload_stats(
    const LzssTansStreamStats& src,
    LzssPayloadStats *dst)
{
    dst->exact_payload_bits = src.exact_payload_bits;
    dst->rounded_payload_bytes = src.rounded_payload_bytes;
    dst->model_header_bits = src.model_header_bits;
    dst->tans_stream_bits = src.tans_stream_bits;
    dst->extra_stream_bits = src.extra_stream_bits;
    dst->padding_bits = src.padding_bits;
    dst->total_stream_bytes = src.total_stream_bytes;
    dst->empirical_entropy_bits = src.empirical_entropy_bits;
    dst->normalization_loss_bits = src.normalization_loss_bits;
    dst->tans_coder_overhead_bits = src.tans_coder_overhead_bits;
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

static bool encode_one_block(
    const uint8_t *input,
    size_t input_size,
    size_t block_size,
    size_t block_count,
    size_t block_index,
    const LzssConfig *config,
    LzssTansTableMode table_mode,
    EncodedBlockResult *result)
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

    TansCodecGuard codec;
    if (!codec.init(config)) {
        sequence_stream_free(&sequence_stream);
        return false;
    }

    if (config->parse_mode == LZSS_PARSE_OPTIMAL) {
        LzssConfig seed_config = *config;
        seed_config.parse_mode = LZSS_PARSE_LAZY;

        LzssSequenceStream seed_stream;
        sequence_stream_init(&seed_stream, 0);

        auto parse_start = std::chrono::steady_clock::now();
        const bool seed_ok =
            lzss_encode(
                block_input,
                current_block_size,
                &seed_config,
                &seed_stream
            );
        result->timings.parse_ms += elapsed_ms_since(parse_start);

        LzssTansCostModel cost_model;
        bool optimal_ok = seed_ok;
        if (optimal_ok) {
            auto model_start = std::chrono::steady_clock::now();
            optimal_ok =
                lzss_tans_build_models(&codec.codec, &seed_stream, true) &&
                lzss_tans_cost_model_init(&codec.codec, &cost_model);
            result->timings.model_build_ms += elapsed_ms_since(model_start);
        }

        if (optimal_ok) {
            parse_start = std::chrono::steady_clock::now();
            optimal_ok = lzss_encode_optimal(
                block_input,
                current_block_size,
                config,
                &cost_model,
                &sequence_stream
            );
            result->timings.parse_ms += elapsed_ms_since(parse_start);
        }

        if (optimal_ok && table_mode == LZSS_TANS_TABLE_REBUILD) {
            const auto model_start = std::chrono::steady_clock::now();
            optimal_ok =
                lzss_tans_build_models(&codec.codec, &sequence_stream, false);
            result->timings.model_build_ms += elapsed_ms_since(model_start);
        }

        sequence_stream_free(&seed_stream);

        if (!optimal_ok) {
            sequence_stream_free(&sequence_stream);
            return false;
        }
    } else {
        const auto parse_start = std::chrono::steady_clock::now();
        const bool parse_ok = lzss_encode(
                block_input,
                current_block_size,
                config,
                &sequence_stream);
        result->timings.parse_ms += elapsed_ms_since(parse_start);

        if (!parse_ok) {
            sequence_stream_free(&sequence_stream);
            return false;
        }

        const auto model_start = std::chrono::steady_clock::now();
        const bool models_ready =
            lzss_tans_build_models(&codec.codec, &sequence_stream, false);
        result->timings.model_build_ms += elapsed_ms_since(model_start);

        if (!models_ready) {
            sequence_stream_free(&sequence_stream);
            return false;
        }
    }

    collect_sequence_stats(sequence_stream, &result->stats);

    const size_t word_count = std::max<size_t>(
        1024,
        sequence_stream.sequences.size() * 8 + current_block_size + 64
    );
    std::vector<uint32_t> compressed_words(word_count, 0);

    bio writer{};
    bio_open(
        &writer,
        compressed_words.data(),
        compressed_words.data() + compressed_words.size(),
        BIO_MODE_WRITE
    );

    LzssTansStreamStats stream_stats{};
    const auto entropy_start = std::chrono::steady_clock::now();
    const bool encoded = lzss_tans_encode_stream_with_current_models(
        &codec.codec,
        &writer,
        &sequence_stream,
        &stream_stats
    );
    result->timings.entropy_encode_ms += elapsed_ms_since(entropy_start);
    bio_close(&writer, BIO_MODE_WRITE);

    sequence_stream_free(&sequence_stream);

    if (!encoded) {
        return false;
    }

    const size_t used_words =
        static_cast<size_t>(writer.ptr - compressed_words.data());
    compressed_words.resize(used_words);
    copy_tans_payload_stats(stream_stats, &result->payload_stats);
    result->payload_stats.rounded_payload_bytes =
        used_words * sizeof(uint32_t);
    result->payload_stats.total_stream_bytes =
        result->payload_stats.rounded_payload_bytes;
    result->compressed_words = std::move(compressed_words);
    return true;
}

static bool decode_one_block(
    const LzssTansBlockStream *stream,
    const LzssConfig *config,
    ByteBuffer *out,
    size_t block_index,
    LzssBlockTimings *timings)
{
    const size_t block_count = stream->blocks.size();
    const size_t expected_size = block_uncompressed_size(
        stream->original_size,
        stream->block_size,
        block_count,
        block_index
    );
    const LzssTansBlock& block = stream->blocks[block_index];

    if (block.compressed_words.empty()) {
        return false;
    }

    TansCodecGuard codec;
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

    const auto entropy_start = std::chrono::steady_clock::now();
    bool ok = lzss_tans_decode_stream(
        &codec.codec,
        &reader,
        &decoded_stream
    );
    if (timings != nullptr) {
        timings->entropy_decode_ms += elapsed_ms_since(entropy_start);
    }

    ByteBuffer decoded_bytes;
    buffer_init(&decoded_bytes);
    buffer_init_with_capacity(&decoded_bytes, expected_size);

    if (ok) {
        const auto reconstruct_start = std::chrono::steady_clock::now();
        ok = lzss_decode(&decoded_stream, &decoded_bytes);
        if (timings != nullptr) {
            timings->reconstruct_ms += elapsed_ms_since(reconstruct_start);
        }
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

void lzss_tans_block_stream_init(LzssTansBlockStream *stream)
{
    if (stream == nullptr) {
        return;
    }

    stream->original_size = 0;
    stream->block_size = LZSS_TANS_DEFAULT_BLOCK_SIZE;
    stream->blocks.clear();
    stream->stats = {};
    stream->payload_stats = {};
    stream->timings = {};
}

void lzss_tans_block_stream_clear(LzssTansBlockStream *stream)
{
    if (stream == nullptr) {
        return;
    }

    LzssTansBlockStream empty{};
    empty.block_size = LZSS_TANS_DEFAULT_BLOCK_SIZE;
    *stream = std::move(empty);
}

size_t lzss_tans_block_stream_compressed_size(
    const LzssTansBlockStream *stream)
{
    if (stream == nullptr) {
        return 0;
    }

    size_t compressed_size = 0;
    for (const LzssTansBlock& block : stream->blocks) {
        compressed_size += block.compressed_words.size() * sizeof(uint32_t);
    }

    return compressed_size;
}

bool lzss_tans_encode_blocks(
    const uint8_t *input,
    size_t input_size,
    size_t block_size,
    size_t max_workers,
    const LzssConfig *config,
    LzssTansTableMode table_mode,
    LzssTansBlockStream *out_stream)
{
    if ((input_size > 0 && input == nullptr) ||
        block_size == 0 ||
        max_workers == 0 ||
        config == nullptr ||
        !is_valid_table_mode(table_mode) ||
        out_stream == nullptr) {
        return false;
    }

    try {
        const size_t block_count =
            block_count_for_size(input_size, block_size);

        LzssTansBlockStream working{};
        working.original_size = input_size;
        working.block_size = block_size;
        working.blocks.resize(block_count);

        std::vector<EncodedBlockResult> results(block_count);
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
                                table_mode,
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
            add_payload_stats(
                &working.payload_stats,
                results[block_index].payload_stats
            );
            add_timings(&working.timings, results[block_index].timings);
        }

        *out_stream = std::move(working);
        return true;
    } catch (...) {
        return false;
    }
}

bool lzss_tans_decode_blocks(
    const LzssTansBlockStream *stream,
    size_t max_workers,
    const LzssConfig *config,
    ByteBuffer *out,
    LzssBlockTimings *timings)
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
        std::vector<LzssBlockTimings> block_timings(stream->blocks.size());

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
                                block_index,
                                &block_timings[block_index])) {
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

        if (timings != nullptr) {
            *timings = {};
            for (const LzssBlockTimings& block_timing : block_timings) {
                add_timings(timings, block_timing);
            }
        }

        out->size = stream->original_size;
        return true;
    } catch (...) {
        out->size = 0;
        return false;
    }
}

#include "lzss_test.h"

#include "block_tans.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ostream>
#include <string>
#include <vector>

struct CompressionResult {
    bool ok;
    size_t token_count;
    size_t match_token_count;
    size_t literal_token_count;
    size_t match_memory;
    size_t match_length_total;
    size_t compressed_size;
    double compress_ms;
    double decompress_ms;
};

static const char *default_entropy_codec_name()
{
    return "tans";
}

static std::vector<uint8_t> read_file(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }

    const std::streamsize size = file.tellg();
    if (size < 0) {
        return {};
    }

    std::vector<uint8_t> data(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);

    if (!data.empty()) {
        file.read(reinterpret_cast<char *>(data.data()), size);
    }

    if (!file && !data.empty()) {
        return {};
    }

    return data;
}

static void copy_block_stats_to_result(
    const LzssBlockStats& stats,
    CompressionResult *result)
{
    result->token_count = stats.token_count;
    result->match_token_count = stats.match_token_count;
    result->literal_token_count = stats.literal_token_count;
    result->match_memory = stats.match_memory;
    result->match_length_total = stats.match_length_total;
}

static CompressionResult compress_decompress_file(
    const std::vector<uint8_t>& input,
    const LzssConfig& config,
    std::ostream& err)
{
    CompressionResult result{};

    LzssTansBlockStream block_stream;
    lzss_tans_block_stream_init(&block_stream);

    const auto compress_start = std::chrono::steady_clock::now();
    bool ok = lzss_tans_encode_blocks(
        input.empty() ? nullptr : input.data(),
        input.size(),
        &config,
        &block_stream
    );
    const auto compress_end = std::chrono::steady_clock::now();

    copy_block_stats_to_result(block_stream.stats, &result);
    result.compressed_size =
        lzss_tans_block_stream_compressed_size(&block_stream);
    result.compress_ms =
        std::chrono::duration<double, std::milli>(
            compress_end - compress_start
        ).count();

    if (!ok) {
        err << "  block tANS compression failed\n";
        lzss_tans_block_stream_clear(&block_stream);
        return result;
    }

    ByteBuffer decoded_bytes;
    buffer_init(&decoded_bytes);
    buffer_init_with_capacity(&decoded_bytes, input.size());

    const auto decompress_start = std::chrono::steady_clock::now();
    ok = lzss_tans_decode_blocks(&block_stream, &config, &decoded_bytes);
    const auto decompress_end = std::chrono::steady_clock::now();

    result.decompress_ms =
        std::chrono::duration<double, std::milli>(
            decompress_end - decompress_start
        ).count();

    const bool same_size = decoded_bytes.size == input.size();
    const bool same_data = same_size &&
        std::equal(input.begin(), input.end(), decoded_bytes.data);

    if (!(ok && same_data)) {
        err << "  block tANS decompression failed or data mismatch"
            << " decoded_size=" << decoded_bytes.size
            << " blocks=" << block_stream.blocks.size() << '\n';
    }

    result.ok = ok && same_data;

    buffer_free(&decoded_bytes);
    lzss_tans_block_stream_clear(&block_stream);
    return result;
}

bool run_silesia_benchmark(std::ostream& out, std::ostream& err)
{
    const std::filesystem::path dataset_dir = "datasets";
    const LzssConfig config{
        65536,
        4,
        258,
        // Future parser work can add a cost-aware mode here.
        LZSS_PARSE_LAZY
    };

    if (!std::filesystem::exists(dataset_dir)) {
        err << "Dataset directory not found: " << dataset_dir.string() << '\n';
        return false;
    }

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dataset_dir)) {
        if (entry.is_regular_file()) {
            files.push_back(entry.path());
        }
    }

    std::sort(files.begin(), files.end());

    if (files.empty()) {
        err << "No dataset files found in " << dataset_dir.string() << '\n';
        return false;
    }

    out << "Silesia benchmark\n";
    out << "Entropy codec = " << default_entropy_codec_name() << "\n";
    out << "Block size = " << LZSS_TANS_BLOCK_SIZE << " bytes\n";
    out << "Compression factor = original_size / compressed_size\n\n";
    out << std::left << std::setw(14) << "file"
        << std::right << std::setw(13) << "input"
        << std::setw(13) << "compressed"
        << std::setw(11) << "factor"
        << std::setw(14) << "comp ms"
        << std::setw(14) << "decomp ms"
        << std::setw(12) << "tokens"
        << std::setw(12) << "matches"
        << std::setw(12) << "literals"
        << std::setw(13) << "match mem"
        << std::setw(12) << "avg match"
        << '\n';

    bool all_ok = true;
    size_t total_input = 0;
    size_t total_compressed = 0;
    size_t total_tokens = 0;
    size_t total_match_tokens = 0;
    size_t total_literal_tokens = 0;
    size_t total_match_memory = 0;
    size_t total_match_length = 0;
    double total_compress_ms = 0.0;
    double total_decompress_ms = 0.0;

    for (const auto& path : files) {
        const std::vector<uint8_t> input = read_file(path);

        if (input.empty() && std::filesystem::file_size(path) != 0) {
            err << "Failed to read " << path.string() << '\n';
            all_ok = false;
            continue;
        }

        const CompressionResult result =
            compress_decompress_file(input, config, err);

        const double factor = result.compressed_size == 0
            ? 0.0
            : static_cast<double>(input.size()) /
              static_cast<double>(result.compressed_size);
        const double avg_match_length = result.match_token_count == 0
            ? 0.0
            : static_cast<double>(result.match_length_total) /
              static_cast<double>(result.match_token_count);

        out << std::left << std::setw(14) << path.filename().string()
            << std::right << std::setw(13) << input.size()
            << std::setw(13) << result.compressed_size
            << std::setw(11) << std::fixed << std::setprecision(3) << factor
            << std::setw(14) << std::fixed << std::setprecision(0)
            << result.compress_ms
            << std::setw(14) << std::fixed << std::setprecision(0)
            << result.decompress_ms
            << std::setw(12) << result.token_count
            << std::setw(12) << result.match_token_count
            << std::setw(12) << result.literal_token_count
            << std::setw(13) << result.match_memory
            << std::setw(12) << std::fixed << std::setprecision(2)
            << avg_match_length
            << (result.ok ? "" : "  FAIL")
            << '\n';

        all_ok = result.ok && all_ok;
        total_input += input.size();
        total_compressed += result.compressed_size;
        total_tokens += result.token_count;
        total_match_tokens += result.match_token_count;
        total_literal_tokens += result.literal_token_count;
        total_match_memory += result.match_memory;
        total_match_length += result.match_length_total;
        total_compress_ms += result.compress_ms;
        total_decompress_ms += result.decompress_ms;
    }

    const double total_factor = total_compressed == 0
        ? 0.0
        : static_cast<double>(total_input) /
          static_cast<double>(total_compressed);
    const double total_avg_match_length = total_match_tokens == 0
        ? 0.0
        : static_cast<double>(total_match_length) /
          static_cast<double>(total_match_tokens);

    out << '\n'
        << std::left << std::setw(14) << "TOTAL"
        << std::right << std::setw(13) << total_input
        << std::setw(13) << total_compressed
        << std::setw(11) << std::fixed << std::setprecision(3)
        << total_factor
        << std::setw(14) << std::fixed << std::setprecision(2)
        << total_compress_ms
        << std::setw(14) << std::fixed << std::setprecision(2)
        << total_decompress_ms
        << std::setw(12) << total_tokens
        << std::setw(12) << total_match_tokens
        << std::setw(12) << total_literal_tokens
        << std::setw(13) << total_match_memory
        << std::setw(12) << std::fixed << std::setprecision(2)
        << total_avg_match_length
        << '\n';

    out << (all_ok ? "OK, all checks passed\n"
                  : "FAIL, some checks failed\n");
    return all_ok;
}

#include "lzss_test.h"

#include "block_ac.h"
#include "block_tans.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

enum EntropyCodec {
    ENTROPY_CODEC_TANS,
    ENTROPY_CODEC_ADAPTIVE_AC
};

struct BenchmarkConfig {
    std::filesystem::path dataset_dir;
    LzssConfig lzss;
    EntropyCodec entropy_codec;
    size_t block_size;
    size_t max_workers;
};

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

static const char *entropy_codec_name(EntropyCodec entropy_codec)
{
    switch (entropy_codec) {
    case ENTROPY_CODEC_TANS:
        return "tans";
    case ENTROPY_CODEC_ADAPTIVE_AC:
        return "ac";
    }

    return "unknown";
}

static std::string trim(const std::string& value)
{
    size_t first = 0;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first]))) {
        ++first;
    }

    size_t last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1]))) {
        --last;
    }

    return value.substr(first, last - first);
}

static std::string to_lower(std::string value)
{
    for (char& ch : value) {
        ch = static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch))
        );
    }
    return value;
}

static bool parse_size(
    const std::unordered_map<std::string, std::string>& values,
    const char *key,
    size_t *out,
    std::ostream& err)
{
    const auto it = values.find(key);
    if (it == values.end() || it->second.empty()) {
        err << "Missing config value: " << key << '\n';
        return false;
    }

    char *end = nullptr;
    const unsigned long long parsed =
        std::strtoull(it->second.c_str(), &end, 10);

    if (end == it->second.c_str() || *end != '\0' ||
        parsed > std::numeric_limits<size_t>::max()) {
        err << "Invalid numeric config value: " << key << '='
            << it->second << '\n';
        return false;
    }

    *out = static_cast<size_t>(parsed);
    return true;
}

static bool parse_parse_mode(
    const std::unordered_map<std::string, std::string>& values,
    LzssParseMode *out,
    std::ostream& err)
{
    const auto it = values.find("parse_mode");
    if (it == values.end() || it->second.empty()) {
        err << "Missing config value: parse_mode\n";
        return false;
    }

    const std::string value = to_lower(it->second);
    if (value == "greedy") {
        *out = LZSS_PARSE_GREEDY;
        return true;
    }
    if (value == "lazy") {
        *out = LZSS_PARSE_LAZY;
        return true;
    }
    if (value == "optimal") {
        *out = LZSS_PARSE_OPTIMAL;
        return true;
    }

    err << "Invalid parse_mode config value: " << it->second << '\n';
    return false;
}

static const char *parse_mode_name(LzssParseMode parse_mode)
{
    switch (parse_mode) {
    case LZSS_PARSE_GREEDY:
        return "greedy";
    case LZSS_PARSE_LAZY:
        return "lazy";
    case LZSS_PARSE_OPTIMAL:
        return "optimal";
    }

    return "unknown";
}

static bool parse_hash_mode(
    const std::unordered_map<std::string, std::string>& values,
    LzssHashMode *out,
    std::ostream& err)
{
    const auto it = values.find("hash_mode");
    if (it == values.end() || it->second.empty()) {
        err << "Missing config value: hash_mode\n";
        return false;
    }

    const std::string value = to_lower(it->second);
    if (value == "hash3") {
        *out = LZSS_HASH3;
        return true;
    }
    if (value == "hash4") {
        *out = LZSS_HASH4;
        return true;
    }

    err << "Invalid hash_mode config value: " << it->second << '\n';
    return false;
}

static const char *hash_mode_name(LzssHashMode hash_mode)
{
    return hash_mode == LZSS_HASH3 ? "hash3" : "hash4";
}

static bool parse_entropy_codec(
    const std::unordered_map<std::string, std::string>& values,
    EntropyCodec *out,
    std::ostream& err)
{
    const auto it = values.find("entropy_codec");
    const std::string value =
        it == values.end() || it->second.empty()
            ? "tans"
            : to_lower(it->second);

    if (value == "tans") {
        *out = ENTROPY_CODEC_TANS;
        return true;
    }
    if (value == "ac" ||
        value == "adaptive_ac" ||
        value == "arithmetic") {
        *out = ENTROPY_CODEC_ADAPTIVE_AC;
        return true;
    }

    err << "Invalid entropy_codec config value: " << it->second << '\n';
    return false;
}

static bool load_benchmark_config(
    const std::filesystem::path& path,
    BenchmarkConfig *config,
    std::ostream& err)
{
    std::ifstream file(path);
    if (!file) {
        err << "Config file not found: " << path.string() << '\n';
        return false;
    }

    std::unordered_map<std::string, std::string> values;
    std::string line;
    size_t line_number = 0;

    while (std::getline(file, line)) {
        ++line_number;
        const size_t comment = line.find('#');
        if (comment != std::string::npos) {
            line.erase(comment);
        }

        line = trim(line);
        if (line.empty()) {
            continue;
        }

        const size_t separator = line.find('=');
        if (separator == std::string::npos) {
            err << "Invalid config line " << line_number << ": "
                << line << '\n';
            return false;
        }

        const std::string key = trim(line.substr(0, separator));
        const std::string value = trim(line.substr(separator + 1));
        if (key.empty()) {
            err << "Invalid empty config key on line " << line_number << '\n';
            return false;
        }

        values[key] = value;
    }

    const auto dataset_it = values.find("dataset_dir");
    if (dataset_it == values.end() || dataset_it->second.empty()) {
        err << "Missing config value: dataset_dir\n";
        return false;
    }

    config->dataset_dir = dataset_it->second;
    if (!parse_size(values, "window_size", &config->lzss.window_size, err) ||
        !parse_size(values, "min_match_length",
                    &config->lzss.min_match_length, err) ||
        !parse_size(values, "max_match_length",
                    &config->lzss.max_match_length, err) ||
        !parse_size(values, "block_size", &config->block_size, err) ||
        !parse_size(values, "max_workers", &config->max_workers, err) ||
        !parse_parse_mode(values, &config->lzss.parse_mode, err) ||
        !parse_hash_mode(values, &config->lzss.hash_mode, err) ||
        !parse_entropy_codec(values, &config->entropy_codec, err)) {
        return false;
    }

    const size_t hash_match_length =
        config->lzss.hash_mode == LZSS_HASH3 ? 3 : 4;
    if (config->lzss.window_size == 0 ||
        config->lzss.min_match_length == 0 ||
        config->lzss.max_match_length < config->lzss.min_match_length ||
        config->block_size == 0 ||
        config->max_workers == 0) {
        err << "Invalid config: sizes must be positive and "
            << "max_match_length must be >= min_match_length\n";
        return false;
    }
    if (config->lzss.min_match_length < hash_match_length) {
        err << "Invalid config: min_match_length must be >= "
            << hash_match_length << " for "
            << hash_mode_name(config->lzss.hash_mode) << '\n';
        return false;
    }

    return true;
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
    EntropyCodec entropy_codec,
    size_t block_size,
    size_t max_workers,
    std::ostream& err)
{
    CompressionResult result{};

    const auto compress_start = std::chrono::steady_clock::now();

    bool ok = false;
    LzssTansBlockStream tans_block_stream;
    LzssAcBlockStream ac_block_stream;
    lzss_tans_block_stream_init(&tans_block_stream);
    lzss_ac_block_stream_init(&ac_block_stream);

    if (entropy_codec == ENTROPY_CODEC_ADAPTIVE_AC) {
        ok = lzss_ac_encode_blocks(
            input.empty() ? nullptr : input.data(),
            input.size(),
            block_size,
            max_workers,
            &config,
            &ac_block_stream
        );
        copy_block_stats_to_result(ac_block_stream.stats, &result);
        result.compressed_size =
            lzss_ac_block_stream_compressed_size(&ac_block_stream);
    } else {
        ok = lzss_tans_encode_blocks(
            input.empty() ? nullptr : input.data(),
            input.size(),
            block_size,
            max_workers,
            &config,
            &tans_block_stream
        );
        copy_block_stats_to_result(tans_block_stream.stats, &result);
        result.compressed_size =
            lzss_tans_block_stream_compressed_size(&tans_block_stream);
    }

    const auto compress_end = std::chrono::steady_clock::now();

    result.compress_ms =
        std::chrono::duration<double, std::milli>(
            compress_end - compress_start
        ).count();

    if (!ok) {
        err << "  block " << entropy_codec_name(entropy_codec)
            << " compression failed\n";
        lzss_tans_block_stream_clear(&tans_block_stream);
        lzss_ac_block_stream_clear(&ac_block_stream);
        return result;
    }

    ByteBuffer decoded_bytes;
    buffer_init(&decoded_bytes);
    buffer_init_with_capacity(&decoded_bytes, input.size());

    const auto decompress_start = std::chrono::steady_clock::now();
    if (entropy_codec == ENTROPY_CODEC_ADAPTIVE_AC) {
        ok = lzss_ac_decode_blocks(
            &ac_block_stream,
            max_workers,
            &config,
            &decoded_bytes
        );
    } else {
        ok = lzss_tans_decode_blocks(
            &tans_block_stream,
            max_workers,
            &config,
            &decoded_bytes
        );
    }
    const auto decompress_end = std::chrono::steady_clock::now();

    result.decompress_ms =
        std::chrono::duration<double, std::milli>(
            decompress_end - decompress_start
        ).count();

    const bool same_size = decoded_bytes.size == input.size();
    const bool same_data = same_size &&
        std::equal(input.begin(), input.end(), decoded_bytes.data);

    if (!(ok && same_data)) {
        const size_t block_count =
            entropy_codec == ENTROPY_CODEC_ADAPTIVE_AC
                ? ac_block_stream.blocks.size()
                : tans_block_stream.blocks.size();
        err << "  block " << entropy_codec_name(entropy_codec)
            << " decompression failed or data mismatch"
            << " decoded_size=" << decoded_bytes.size
            << " blocks=" << block_count << '\n';
    }

    result.ok = ok && same_data;

    buffer_free(&decoded_bytes);
    lzss_tans_block_stream_clear(&tans_block_stream);
    lzss_ac_block_stream_clear(&ac_block_stream);
    return result;
}

bool run_silesia_benchmark(std::ostream& out, std::ostream& err)
{
    BenchmarkConfig benchmark_config{};
    if (!load_benchmark_config("lzss.conf", &benchmark_config, err)) {
        return false;
    }

    const std::filesystem::path& dataset_dir = benchmark_config.dataset_dir;
    const LzssConfig& config = benchmark_config.lzss;

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
    out << "Entropy codec = "
        << entropy_codec_name(benchmark_config.entropy_codec) << "\n";
    out << "Dataset directory = " << dataset_dir.string() << "\n";
    out << "Window size = " << config.window_size << " bytes\n";
    out << "Match length = " << config.min_match_length << ".."
        << config.max_match_length << " bytes\n";
    out << "Parse mode = " << parse_mode_name(config.parse_mode) << "\n";
    out << "Hash mode = " << hash_mode_name(config.hash_mode) << "\n";
    out << "Block size = " << benchmark_config.block_size << " bytes\n";
    out << "Max workers = " << benchmark_config.max_workers << "\n";
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
            compress_decompress_file(
                input,
                config,
                benchmark_config.entropy_codec,
                benchmark_config.block_size,
                benchmark_config.max_workers,
                err
            );

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

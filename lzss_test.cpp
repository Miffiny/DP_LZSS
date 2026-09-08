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
#include <sstream>
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
    bool stats_consistent;
    size_t block_count;
    size_t token_count;
    size_t match_token_count;
    size_t literal_token_count;
    size_t match_memory;
    size_t match_length_total;
    size_t compressed_size;
    double compress_ms;
    double decompress_ms;
};

struct DerivedCompressionStats {
    double bits_per_byte;
    double compression_factor;
    double saving_percent;
    double compression_mib_per_second;
    double decompression_mib_per_second;
    double match_coverage_percent;
    double literal_coverage_percent;
    double average_match_length;
    double average_literals_per_sequence;
    double matches_per_kib;
};

static double throughput_mib_per_second(size_t input_size, double elapsed_ms)
{
    if (input_size == 0 || elapsed_ms <= 0.0) {
        return 0.0;
    }

    const double input_mib =
        static_cast<double>(input_size) / (1024.0 * 1024.0);
    const double elapsed_seconds = elapsed_ms / 1000.0;
    return input_mib / elapsed_seconds;
}

static DerivedCompressionStats calculate_derived_stats(
    size_t input_size,
    const CompressionResult& result)
{
    DerivedCompressionStats stats{};

    if (input_size > 0) {
        stats.bits_per_byte =
            8.0 * static_cast<double>(result.compressed_size) /
            static_cast<double>(input_size);
        stats.saving_percent =
            100.0 *
            (1.0 -
             static_cast<double>(result.compressed_size) /
             static_cast<double>(input_size));
        stats.match_coverage_percent =
            100.0 * static_cast<double>(result.match_length_total) /
            static_cast<double>(input_size);
        stats.literal_coverage_percent =
            100.0 * static_cast<double>(result.literal_token_count) /
            static_cast<double>(input_size);
        stats.matches_per_kib =
            1024.0 * static_cast<double>(result.match_token_count) /
            static_cast<double>(input_size);
    }

    if (result.compressed_size > 0) {
        stats.compression_factor =
            static_cast<double>(input_size) /
            static_cast<double>(result.compressed_size);
    }

    if (result.match_token_count > 0) {
        stats.average_match_length =
            static_cast<double>(result.match_length_total) /
            static_cast<double>(result.match_token_count);
    }

    if (result.token_count > 0) {
        stats.average_literals_per_sequence =
            static_cast<double>(result.literal_token_count) /
            static_cast<double>(result.token_count);
    }

    stats.compression_mib_per_second =
        throughput_mib_per_second(input_size, result.compress_ms);
    stats.decompression_mib_per_second =
        throughput_mib_per_second(input_size, result.decompress_ms);

    return stats;
}

static bool represented_size_is_consistent(
    size_t input_size,
    size_t literal_byte_count,
    size_t matched_byte_count,
    size_t *represented_size)
{
    if (literal_byte_count >
        std::numeric_limits<size_t>::max() - matched_byte_count) {
        return false;
    }

    const size_t represented = literal_byte_count + matched_byte_count;
    if (represented_size != nullptr) {
        *represented_size = represented;
    }

    return represented == input_size;
}

static bool sequence_statistics_are_consistent(
    size_t input_size,
    const CompressionResult& result)
{
    return represented_size_is_consistent(
        input_size,
        result.literal_token_count,
        result.match_length_total,
        nullptr
    );
}

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
    if (value == "adaptive_optimal" ||
        value == "adaptive-optimal" ||
        value == "ac_optimal" ||
        value == "ac-optimal") {
        *out = LZSS_PARSE_ADAPTIVE_OPTIMAL;
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
    case LZSS_PARSE_ADAPTIVE_OPTIMAL:
        return "adaptive_optimal";
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

static bool parse_distance_coding(
    const std::unordered_map<std::string, std::string>& values,
    LzssDistanceCodingMode *out,
    std::ostream& err)
{
    const auto it = values.find("distance_coding");
    const std::string value =
        it == values.end() || it->second.empty()
            ? "class"
            : to_lower(it->second);

    if (value == "class" || value == "classes") {
        *out = LZSS_DISTANCE_CLASS;
        return true;
    }
    if (value == "bit_tree" ||
        value == "bittree" ||
        value == "tree") {
        *out = LZSS_DISTANCE_BIT_TREE;
        return true;
    }

    err << "Invalid distance_coding config value: " << it->second << '\n';
    return false;
}

static const char *distance_coding_name(
    LzssDistanceCodingMode distance_coding)
{
    switch (distance_coding) {
    case LZSS_DISTANCE_CLASS:
        return "class";
    case LZSS_DISTANCE_BIT_TREE:
        return "bit_tree";
    }

    return "unknown";
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
        !parse_distance_coding(values, &config->lzss.distance_coding, err) ||
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
    if (config->entropy_codec != ENTROPY_CODEC_ADAPTIVE_AC &&
        config->lzss.parse_mode == LZSS_PARSE_ADAPTIVE_OPTIMAL) {
        err << "Invalid config: adaptive_optimal parse_mode requires "
            << "entropy_codec=ac\n";
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
    result.block_count = input.empty()
        ? 1
        : (input.size() - 1) / block_size + 1;

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

    result.stats_consistent =
        sequence_statistics_are_consistent(input.size(), result);

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

    if (ok && same_data && !result.stats_consistent) {
        err << "  inconsistent LZSS statistics:"
            << " input=" << input.size()
            << " literals=" << result.literal_token_count
            << " matched=" << result.match_length_total;

        if (result.literal_token_count <=
            std::numeric_limits<size_t>::max() -
                result.match_length_total) {
            const size_t represented_size =
                result.literal_token_count + result.match_length_total;
            err << " represented=" << represented_size;
        } else {
            err << " represented=overflow";
        }

        err << '\n';
    }

    result.ok = ok && same_data && result.stats_consistent;

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

    std::ostringstream report;
    auto emit_report = [&](const std::string& text) {
        report << text;
        out << text;
        out.flush();
    };

    std::ostringstream header;
    header << "Silesia benchmark\n";
    header << "Entropy codec = "
        << entropy_codec_name(benchmark_config.entropy_codec) << "\n";
    header << "Dataset directory = " << dataset_dir.string() << "\n";
    header << "Window size = " << config.window_size << " bytes\n";
    header << "Match length = " << config.min_match_length << ".."
        << config.max_match_length << " bytes\n";
    header << "Parse mode = " << parse_mode_name(config.parse_mode) << "\n";
    header << "Hash mode = " << hash_mode_name(config.hash_mode) << "\n";
    header << "Distance coding = "
        << distance_coding_name(config.distance_coding) << "\n";
    header << "Block size = " << benchmark_config.block_size << " bytes\n";
    header << "Max workers = " << benchmark_config.max_workers << "\n";
    header << "Compression factor = input bytes / payload bytes\n";
    header << "Payload size is rounded to complete 32-bit words and does "
              "not include an outer container header\n\n";
    header << std::left << std::setw(14) << "file"
        << std::right << std::setw(12) << "input"
        << std::setw(12) << "payload"
        << std::setw(9) << "bit/B"
        << std::setw(9) << "factor"
        << std::setw(9) << "save %"
        << std::setw(12) << "C MiB/s"
        << std::setw(12) << "D MiB/s"
        << std::setw(9) << "blocks"
        << std::setw(12) << "sequences"
        << std::setw(12) << "matches"
        << std::setw(12) << "literal B"
        << std::setw(13) << "matched B"
        << std::setw(10) << "match %"
        << std::setw(11) << "avg match"
        << '\n';
    emit_report(header.str());

    bool all_ok = true;
    size_t total_input = 0;
    size_t total_compressed = 0;
    size_t total_blocks = 0;
    size_t total_tokens = 0;
    size_t total_match_tokens = 0;
    size_t total_literal_tokens = 0;
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

        const DerivedCompressionStats derived =
            calculate_derived_stats(input.size(), result);

        std::ostringstream row;
        row << std::left << std::setw(14) << path.filename().string()
            << std::right << std::setw(12) << input.size()
            << std::setw(12) << result.compressed_size
            << std::setw(9) << std::fixed << std::setprecision(3)
            << derived.bits_per_byte
            << std::setw(9) << std::fixed << std::setprecision(3)
            << derived.compression_factor
            << std::setw(9) << std::fixed << std::setprecision(2)
            << derived.saving_percent
            << std::setw(12) << std::fixed << std::setprecision(2)
            << derived.compression_mib_per_second
            << std::setw(12) << std::fixed << std::setprecision(2)
            << derived.decompression_mib_per_second
            << std::setw(9) << result.block_count
            << std::setw(12) << result.token_count
            << std::setw(12) << result.match_token_count
            << std::setw(12) << result.literal_token_count
            << std::setw(13) << result.match_length_total
            << std::setw(10) << std::fixed << std::setprecision(2)
            << derived.match_coverage_percent
            << std::setw(11) << std::fixed << std::setprecision(2)
            << derived.average_match_length
            << (result.ok ? "" : "  FAIL")
            << '\n';
        emit_report(row.str());

        all_ok = result.ok && all_ok;
        total_input += input.size();
        total_compressed += result.compressed_size;
        total_blocks += result.block_count;
        total_tokens += result.token_count;
        total_match_tokens += result.match_token_count;
        total_literal_tokens += result.literal_token_count;
        total_match_length += result.match_length_total;
        total_compress_ms += result.compress_ms;
        total_decompress_ms += result.decompress_ms;
    }

    CompressionResult total_result{};
    total_result.ok = all_ok;
    total_result.stats_consistent = represented_size_is_consistent(
        total_input,
        total_literal_tokens,
        total_match_length,
        nullptr
    );
    total_result.block_count = total_blocks;
    total_result.token_count = total_tokens;
    total_result.match_token_count = total_match_tokens;
    total_result.literal_token_count = total_literal_tokens;
    total_result.match_length_total = total_match_length;
    total_result.compressed_size = total_compressed;
    total_result.compress_ms = total_compress_ms;
    total_result.decompress_ms = total_decompress_ms;

    const DerivedCompressionStats total_derived =
        calculate_derived_stats(total_input, total_result);

    std::ostringstream summary;
    summary << '\n'
        << std::left << std::setw(14) << "TOTAL"
        << std::right << std::setw(12) << total_input
        << std::setw(12) << total_compressed
        << std::setw(9) << std::fixed << std::setprecision(3)
        << total_derived.bits_per_byte
        << std::setw(9) << std::fixed << std::setprecision(3)
        << total_derived.compression_factor
        << std::setw(9) << std::fixed << std::setprecision(2)
        << total_derived.saving_percent
        << std::setw(12) << std::fixed << std::setprecision(2)
        << total_derived.compression_mib_per_second
        << std::setw(12) << std::fixed << std::setprecision(2)
        << total_derived.decompression_mib_per_second
        << std::setw(9) << total_blocks
        << std::setw(12) << total_tokens
        << std::setw(12) << total_match_tokens
        << std::setw(12) << total_literal_tokens
        << std::setw(13) << total_match_length
        << std::setw(10) << std::fixed << std::setprecision(2)
        << total_derived.match_coverage_percent
        << std::setw(11) << std::fixed << std::setprecision(2)
        << total_derived.average_match_length
        << '\n';

    if (!total_result.stats_consistent) {
        all_ok = false;
        summary << "FAIL, corpus LZSS statistics are inconsistent\n";
    } else {
        summary << (all_ok ? "OK, all checks passed\n"
                           : "FAIL, some checks failed\n");
    }
    emit_report(summary.str());

    const std::string report_text = report.str();

    std::ofstream history("Experiment_runs.txt", std::ios::app);
    if (history) {
        history << "\n\n" << report_text;
    } else {
        err << "Failed to append benchmark result to Experiment runs.txt\n";
    }

    return all_ok;
}

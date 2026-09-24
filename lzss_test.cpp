#include "lzss_test.h"

#include "block_ac.h"
#include "block_tans.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <ostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

enum EntropyCodec {
    ENTROPY_CODEC_TANS,
    ENTROPY_CODEC_ADAPTIVE_AC
};

struct BenchmarkConfig {
    std::filesystem::path dataset_dir;
    LzssConfig lzss;
    EntropyCodec entropy_codec;
    LzssTansTableMode tans_table_mode;
    size_t block_size;
    size_t max_workers;
};

struct CompressionResult {
    bool ok;
    bool content_match;
    bool stats_consistent;
    size_t block_count;
    size_t token_count;
    size_t match_token_count;
    size_t literal_token_count;
    size_t match_memory;
    size_t match_length_total;
    size_t rep0_count;
    size_t rep1_count;
    size_t rep2_count;
    size_t new_distance_count;
    size_t compressed_size;
    LzssPayloadStats payload_stats;
    LzssBlockTimings timings;
    double compress_ms;
    double decompress_ms;
    size_t peak_rss_bytes;
    std::string status;
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
    double repeat_distance_hit_percent;
};

static const char *entropy_codec_name(EntropyCodec entropy_codec);
static const char *parse_mode_name(LzssParseMode parse_mode);
static const char *tans_table_mode_name(LzssTansTableMode table_mode);
static std::string benchmark_mode_name(const BenchmarkConfig& config);

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
            static_cast<double>(result.payload_stats.exact_payload_bits) /
            static_cast<double>(input_size);
        stats.saving_percent =
            100.0 *
            (1.0 -
             static_cast<double>(result.payload_stats.rounded_payload_bytes) /
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

    if (result.payload_stats.rounded_payload_bytes > 0) {
        stats.compression_factor =
            static_cast<double>(input_size) /
            static_cast<double>(result.payload_stats.rounded_payload_bytes);
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

    if (result.match_token_count > 0) {
        const size_t repeat_distance_hits =
            result.rep0_count + result.rep1_count + result.rep2_count;
        stats.repeat_distance_hit_percent =
            100.0 * static_cast<double>(repeat_distance_hits) /
            static_cast<double>(result.match_token_count);
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

static size_t current_peak_rss_bytes()
{
#ifdef _WIN32
    using GetProcessMemoryInfoFn =
        BOOL (WINAPI *)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);

    auto load_memory_info_proc = [](HMODULE module, const char *name) {
        GetProcessMemoryInfoFn fn = nullptr;
        if (module == nullptr) {
            return fn;
        }

        FARPROC proc = GetProcAddress(module, name);
        if (proc != nullptr && sizeof(proc) == sizeof(fn)) {
            std::memcpy(&fn, &proc, sizeof(fn));
        }

        return fn;
    };

    GetProcessMemoryInfoFn get_process_memory_info = nullptr;

    HMODULE psapi = GetModuleHandleA("psapi.dll");
    if (psapi == nullptr) {
        psapi = LoadLibraryA("psapi.dll");
    }
    if (psapi != nullptr) {
        get_process_memory_info =
            load_memory_info_proc(psapi, "GetProcessMemoryInfo");
    }

    if (get_process_memory_info == nullptr) {
        HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
        if (kernel32 != nullptr) {
            get_process_memory_info =
                load_memory_info_proc(kernel32, "K32GetProcessMemoryInfo");
        }
    }

    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (get_process_memory_info != nullptr &&
        get_process_memory_info(
            GetCurrentProcess(),
            &counters,
            sizeof(counters))) {
        return static_cast<size_t>(counters.PeakWorkingSetSize);
    }

    return 0;
#else
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0;
    }

#if defined(__APPLE__)
    return static_cast<size_t>(usage.ru_maxrss);
#else
    return static_cast<size_t>(usage.ru_maxrss) * 1024u;
#endif
#endif
}

static const char *csv_header_text()
{
    return
        "mode,entropy_codec,parse_mode,tans_table_mode,"
        "window_size,min_match_length,max_match_length,"
        "max_chain_length,good_match_length,optimal_max_chain_length,"
        "hash_size,tans_table_log,max_workers,"
        "file,input_bytes,block_size,blocks,"
        "exact_payload_bits,rounded_payload_bytes,model_header_bits,"
        "tans_stream_bits,extra_stream_bits,padding_bits,"
        "total_stream_bytes,"
        "parse_ms,model_build_ms,tans_encode_ms,total_compress_ms,"
        "tans_decode_ms,lzss_reconstruct_ms,total_decompress_ms,"
        "bits_per_byte,compression_factor,compression_mib_s,"
        "decompression_mib_s,peak_rss_bytes,"
        "sequences,literal_bytes,matched_bytes,matches,"
        "avg_match_length,avg_literals_per_sequence,"
        "rep0_count,rep1_count,rep2_count,new_distance_count,"
        "repeat_distance_hit_pct,"
        "empirical_entropy_bits,normalization_loss_bits,"
        "tans_coder_overhead_bits,content_match,status";
}

static bool csv_file_needs_header(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file || file.peek() == std::ifstream::traits_type::eof()) {
        return true;
    }

    std::string first_line;
    std::getline(file, first_line);
    if (!first_line.empty() && first_line.back() == '\r') {
        first_line.pop_back();
    }

    return first_line != csv_header_text();
}

static void write_csv_escaped(std::ostream& csv, const std::string& value)
{
    const bool needs_quotes =
        value.find_first_of(",\"\r\n") != std::string::npos;
    if (!needs_quotes) {
        csv << value;
        return;
    }

    csv << '"';
    for (char ch : value) {
        if (ch == '"') {
            csv << "\"\"";
        } else {
            csv << ch;
        }
    }
    csv << '"';
}

static void write_csv_header(std::ostream& csv)
{
    csv << csv_header_text() << '\n';
}

static void write_csv_row(
    std::ostream& csv,
    const BenchmarkConfig& config,
    const std::string& file_name,
    size_t input_size,
    size_t block_size,
    const CompressionResult& result,
    const DerivedCompressionStats& derived)
{
    const std::streamsize old_precision = csv.precision();
    const std::ios::fmtflags old_flags = csv.flags();

    csv << std::fixed << std::setprecision(6);
    write_csv_escaped(csv, benchmark_mode_name(config));
    csv << ',';
    write_csv_escaped(csv, entropy_codec_name(config.entropy_codec));
    csv << ',';
    write_csv_escaped(csv, parse_mode_name(config.lzss.parse_mode));
    csv << ',';
    write_csv_escaped(
        csv,
        config.entropy_codec == ENTROPY_CODEC_TANS
            ? tans_table_mode_name(config.tans_table_mode)
            : ""
    );
    csv << ',';
    csv << config.lzss.window_size << ','
        << config.lzss.min_match_length << ','
        << config.lzss.max_match_length << ','
        << config.lzss.max_chain_length << ','
        << config.lzss.good_match_length << ','
        << config.lzss.optimal_max_chain_length << ','
        << config.lzss.hash_size << ','
        << config.lzss.tans_table_log << ','
        << config.max_workers << ',';
    write_csv_escaped(csv, file_name);
    csv << ','
        << input_size << ','
        << block_size << ','
        << result.block_count << ','
        << result.payload_stats.exact_payload_bits << ','
        << result.payload_stats.rounded_payload_bytes << ','
        << result.payload_stats.model_header_bits << ','
        << result.payload_stats.tans_stream_bits << ','
        << result.payload_stats.extra_stream_bits << ','
        << result.payload_stats.padding_bits << ','
        << result.payload_stats.total_stream_bytes << ','
        << result.timings.parse_ms << ','
        << result.timings.model_build_ms << ','
        << result.timings.entropy_encode_ms << ','
        << result.compress_ms << ','
        << result.timings.entropy_decode_ms << ','
        << result.timings.reconstruct_ms << ','
        << result.decompress_ms << ','
        << derived.bits_per_byte << ','
        << derived.compression_factor << ','
        << derived.compression_mib_per_second << ','
        << derived.decompression_mib_per_second << ','
        << result.peak_rss_bytes << ','
        << result.token_count << ','
        << result.literal_token_count << ','
        << result.match_length_total << ','
        << result.match_token_count << ','
        << derived.average_match_length << ','
        << derived.average_literals_per_sequence << ','
        << result.rep0_count << ','
        << result.rep1_count << ','
        << result.rep2_count << ','
        << result.new_distance_count << ','
        << derived.repeat_distance_hit_percent << ','
        << result.payload_stats.empirical_entropy_bits << ','
        << result.payload_stats.normalization_loss_bits << ','
        << result.payload_stats.tans_coder_overhead_bits << ','
        << (result.content_match ? 1 : 0) << ',';
    write_csv_escaped(csv, result.status);
    csv << '\n';

    csv.flags(old_flags);
    csv.precision(old_precision);
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

static const char *tans_table_mode_name(LzssTansTableMode table_mode)
{
    switch (table_mode) {
    case LZSS_TANS_TABLE_LAZY:
        return "lazy";
    case LZSS_TANS_TABLE_REBUILD:
        return "rebuild";
    }

    return "unknown";
}

static std::string benchmark_mode_name(const BenchmarkConfig& config)
{
    if (config.entropy_codec == ENTROPY_CODEC_TANS) {
        if (config.lzss.parse_mode == LZSS_PARSE_OPTIMAL &&
            config.tans_table_mode == LZSS_TANS_TABLE_REBUILD) {
            return "optimal_rebuild";
        }

        return parse_mode_name(config.lzss.parse_mode);
    }

    std::string name = entropy_codec_name(config.entropy_codec);
    name += "_";
    name += parse_mode_name(config.lzss.parse_mode);
    return name;
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

static bool parse_optional_size(
    const std::unordered_map<std::string, std::string>& values,
    const char *key,
    size_t default_value,
    size_t *out,
    std::ostream& err)
{
    const auto it = values.find(key);
    if (it == values.end() || it->second.empty()) {
        *out = default_value;
        return true;
    }

    return parse_size(values, key, out, err);
}

static bool is_power_of_two(size_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
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

static bool parse_tans_table_mode(
    const std::unordered_map<std::string, std::string>& values,
    LzssTansTableMode *out,
    std::ostream& err)
{
    const auto it = values.find("tans_table_mode");
    const std::string value =
        it == values.end() || it->second.empty()
            ? "lazy"
            : to_lower(it->second);

    if (value == "lazy") {
        *out = LZSS_TANS_TABLE_LAZY;
        return true;
    }
    if (value == "rebuild") {
        *out = LZSS_TANS_TABLE_REBUILD;
        return true;
    }

    err << "Invalid tans_table_mode config value: " << it->second << '\n';
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
        !parse_optional_size(values, "max_chain_length",
                    LZSS_DEFAULT_MAX_CHAIN_LENGTH,
                    &config->lzss.max_chain_length, err) ||
        !parse_optional_size(values, "good_match_length",
                    LZSS_DEFAULT_GOOD_MATCH_LENGTH,
                    &config->lzss.good_match_length, err) ||
        !parse_optional_size(values, "optimal_max_chain_length",
                    LZSS_DEFAULT_OPTIMAL_MAX_CHAIN_LENGTH,
                    &config->lzss.optimal_max_chain_length, err) ||
        !parse_optional_size(values, "hash_size",
                    LZSS_DEFAULT_HASH_SIZE,
                    &config->lzss.hash_size, err) ||
        !parse_optional_size(values, "tans_table_log",
                    LZSS_DEFAULT_TANS_TABLE_LOG,
                    &config->lzss.tans_table_log, err) ||
        !parse_size(values, "block_size", &config->block_size, err) ||
        !parse_size(values, "max_workers", &config->max_workers, err) ||
        !parse_parse_mode(values, &config->lzss.parse_mode, err) ||
        !parse_hash_mode(values, &config->lzss.hash_mode, err) ||
        !parse_distance_coding(values, &config->lzss.distance_coding, err) ||
        !parse_entropy_codec(values, &config->entropy_codec, err) ||
        !parse_tans_table_mode(values, &config->tans_table_mode, err)) {
        return false;
    }

    const size_t hash_match_length =
        config->lzss.hash_mode == LZSS_HASH3 ? 3 : 4;
    if (config->lzss.window_size == 0 ||
        config->lzss.min_match_length == 0 ||
        config->lzss.max_match_length < config->lzss.min_match_length ||
        config->lzss.max_chain_length == 0 ||
        config->lzss.good_match_length == 0 ||
        config->lzss.optimal_max_chain_length == 0 ||
        config->lzss.hash_size == 0 ||
        config->block_size == 0 ||
        config->max_workers == 0) {
        err << "Invalid config: sizes must be positive and "
            << "max_match_length must be >= min_match_length\n";
        return false;
    }
    if (!is_power_of_two(config->lzss.hash_size)) {
        err << "Invalid config: hash_size must be a power of two\n";
        return false;
    }
    if (config->lzss.tans_table_log < 8 ||
        config->lzss.tans_table_log > 16) {
        err << "Invalid config: tans_table_log must be in 8..16\n";
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
    result->rep0_count = stats.rep0_count;
    result->rep1_count = stats.rep1_count;
    result->rep2_count = stats.rep2_count;
    result->new_distance_count = stats.new_distance_count;
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

static CompressionResult compress_decompress_file(
    const std::vector<uint8_t>& input,
    const LzssConfig& config,
    EntropyCodec entropy_codec,
    size_t block_size,
    size_t max_workers,
    LzssTansTableMode tans_table_mode,
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
        result.payload_stats = ac_block_stream.payload_stats;
        result.timings = ac_block_stream.timings;
        result.compressed_size =
            lzss_ac_block_stream_compressed_size(&ac_block_stream);
    } else {
        ok = lzss_tans_encode_blocks(
            input.empty() ? nullptr : input.data(),
            input.size(),
            block_size,
            max_workers,
            &config,
            tans_table_mode,
            &tans_block_stream
        );
        copy_block_stats_to_result(tans_block_stream.stats, &result);
        result.payload_stats = tans_block_stream.payload_stats;
        result.timings = tans_block_stream.timings;
        result.compressed_size =
            lzss_tans_block_stream_compressed_size(&tans_block_stream);
    }

    if (result.payload_stats.rounded_payload_bytes == 0 &&
        result.compressed_size > 0) {
        result.payload_stats.rounded_payload_bytes = result.compressed_size;
        result.payload_stats.total_stream_bytes = result.compressed_size;
        result.payload_stats.exact_payload_bits =
            result.compressed_size * 8u;
    }

    const auto compress_end = std::chrono::steady_clock::now();

    result.compress_ms =
        std::chrono::duration<double, std::milli>(
            compress_end - compress_start
        ).count();

    if (!ok) {
        result.status = "compression_failed";
        result.peak_rss_bytes = current_peak_rss_bytes();
        err << "  block " << entropy_codec_name(entropy_codec)
            << " compression failed\n";
        lzss_tans_block_stream_clear(&tans_block_stream);
        lzss_ac_block_stream_clear(&ac_block_stream);
        return result;
    }

    ByteBuffer decoded_bytes;
    buffer_init(&decoded_bytes);
    buffer_init_with_capacity(&decoded_bytes, input.size());

    LzssBlockTimings decode_timings{};
    const auto decompress_start = std::chrono::steady_clock::now();
    if (entropy_codec == ENTROPY_CODEC_ADAPTIVE_AC) {
        ok = lzss_ac_decode_blocks(
            &ac_block_stream,
            max_workers,
            &config,
            &decoded_bytes,
            &decode_timings
        );
    } else {
        ok = lzss_tans_decode_blocks(
            &tans_block_stream,
            max_workers,
            &config,
            &decoded_bytes,
            &decode_timings
        );
    }
    const auto decompress_end = std::chrono::steady_clock::now();

    result.decompress_ms =
        std::chrono::duration<double, std::milli>(
            decompress_end - decompress_start
        ).count();
    add_timings(&result.timings, decode_timings);

    const bool same_size = decoded_bytes.size == input.size();
    const bool same_data = same_size &&
        std::equal(input.begin(), input.end(), decoded_bytes.data);
    result.content_match = same_data;

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
    if (result.ok) {
        result.status = "ok";
    } else if (!ok) {
        result.status = "decompression_failed";
    } else if (!same_data) {
        result.status = "content_mismatch";
    } else {
        result.status = "stats_mismatch";
    }

    buffer_free(&decoded_bytes);
    lzss_tans_block_stream_clear(&tans_block_stream);
    lzss_ac_block_stream_clear(&ac_block_stream);
    result.peak_rss_bytes = current_peak_rss_bytes();
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

    auto emit_report = [&](const std::string& text) {
        out << text;
        out.flush();
    };

    const std::filesystem::path csv_path = "Experiment_runs.csv";
    const bool needs_csv_header = csv_file_needs_header(csv_path);
    std::ofstream csv(csv_path, std::ios::app);
    if (csv) {
        if (needs_csv_header) {
            write_csv_header(csv);
        }
    } else {
        err << "Failed to append benchmark result to "
            << csv_path.string() << '\n';
    }

    std::ostringstream header;
    header << "Silesia benchmark\n";
    header << "Entropy codec = "
        << entropy_codec_name(benchmark_config.entropy_codec) << "\n";
    header << "Dataset directory = " << dataset_dir.string() << "\n";
    header << "Window size = " << config.window_size << " bytes\n";
    header << "Match length = " << config.min_match_length << ".."
        << config.max_match_length << " bytes\n";
    header << "Max chain length = " << config.max_chain_length << "\n";
    header << "Good match length = " << config.good_match_length << "\n";
    header << "Optimal max chain length = "
        << config.optimal_max_chain_length << "\n";
    header << "Hash size = " << config.hash_size << "\n";
    header << "Parse mode = " << parse_mode_name(config.parse_mode) << "\n";
    if (benchmark_config.entropy_codec == ENTROPY_CODEC_TANS) {
        header << "tANS table mode = "
            << tans_table_mode_name(benchmark_config.tans_table_mode)
            << "\n";
        header << "tANS table log = " << config.tans_table_log << "\n";
    }
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
    bool all_content_match = true;
    size_t total_input = 0;
    size_t total_compressed = 0;
    size_t total_blocks = 0;
    size_t total_tokens = 0;
    size_t total_match_tokens = 0;
    size_t total_literal_tokens = 0;
    size_t total_match_length = 0;
    size_t total_rep0_count = 0;
    size_t total_rep1_count = 0;
    size_t total_rep2_count = 0;
    size_t total_new_distance_count = 0;
    size_t peak_rss_bytes = 0;
    LzssPayloadStats total_payload_stats{};
    LzssBlockTimings total_timings{};
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
                benchmark_config.tans_table_mode,
                err
            );

        const DerivedCompressionStats derived =
            calculate_derived_stats(input.size(), result);

        if (csv) {
            write_csv_row(
                csv,
                benchmark_config,
                path.filename().string(),
                input.size(),
                benchmark_config.block_size,
                result,
                derived
            );
        }

        std::ostringstream row;
        row << std::left << std::setw(14) << path.filename().string()
            << std::right << std::setw(12) << input.size()
            << std::setw(12) << result.payload_stats.rounded_payload_bytes
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
        all_content_match = result.content_match && all_content_match;
        total_input += input.size();
        total_compressed += result.payload_stats.rounded_payload_bytes;
        total_blocks += result.block_count;
        total_tokens += result.token_count;
        total_match_tokens += result.match_token_count;
        total_literal_tokens += result.literal_token_count;
        total_match_length += result.match_length_total;
        total_rep0_count += result.rep0_count;
        total_rep1_count += result.rep1_count;
        total_rep2_count += result.rep2_count;
        total_new_distance_count += result.new_distance_count;
        peak_rss_bytes = std::max(peak_rss_bytes, result.peak_rss_bytes);
        add_payload_stats(&total_payload_stats, result.payload_stats);
        add_timings(&total_timings, result.timings);
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
    total_result.rep0_count = total_rep0_count;
    total_result.rep1_count = total_rep1_count;
    total_result.rep2_count = total_rep2_count;
    total_result.new_distance_count = total_new_distance_count;
    total_result.compressed_size = total_compressed;
    total_result.payload_stats = total_payload_stats;
    total_result.timings = total_timings;
    total_result.compress_ms = total_compress_ms;
    total_result.decompress_ms = total_decompress_ms;
    total_result.content_match = all_content_match;
    total_result.peak_rss_bytes = peak_rss_bytes;

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

    total_result.ok = all_ok && total_result.stats_consistent;
    total_result.status = total_result.ok ? "ok" : "fail";
    if (csv) {
        write_csv_row(
            csv,
            benchmark_config,
            "TOTAL",
            total_input,
            benchmark_config.block_size,
            total_result,
            total_derived
        );
    }

    emit_report(summary.str());

    return all_ok;
}

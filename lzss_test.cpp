//TODO make better tests, base generated tests are implemented
#include "lzss_test.h"

#include "LZSS/decoder.h"
#include "LZSS/parser.h"
#include "arithmetic.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <utility>
#include <vector>

static bool run_arithmetic_roundtrip(const LzssTokenStream& stream,
                                     const std::vector<uint8_t>& input,
                                     const LzssConfig& config,
                                     std::ostream& err)
{
    LzssArithmeticCodec encoder_codec{};
    LzssArithmeticCodec decoder_codec{};

    if (!lzss_ac_codec_init(&encoder_codec, &config)) {
        err << "  arithmetic encoder codec init failed\n";
        return false;
    }

    const size_t word_count = std::max<size_t>(
        1024,
        stream.count * 8 + input.size() + 64
    );
    std::vector<uint32_t> compressed(word_count, 0);

    ac encoder{};
    bio writer{};
    bio_open(
        &writer,
        compressed.data(),
        compressed.data() + compressed.size(),
        BIO_MODE_WRITE
    );

    const bool encoded = lzss_ac_encode_stream(
        &encoder_codec,
        &encoder,
        &writer,
        &stream
    );
    bio_close(&writer, BIO_MODE_WRITE);

    lzss_ac_codec_destroy(&encoder_codec);

    if (!encoded) {
        err << "  arithmetic encode failed\n";
        return false;
    }

    const size_t used_words =
        static_cast<size_t>(writer.ptr - compressed.data());

    if (!lzss_ac_codec_init(&decoder_codec, &config)) {
        err << "  arithmetic decoder codec init failed\n";
        return false;
    }

    LzssTokenStream decoded_stream;
    token_stream_init(&decoded_stream, 0);

    ac decoder{};
    bio reader{};
    bio_open(
        &reader,
        compressed.data(),
        compressed.data() + used_words,
        BIO_MODE_READ
    );

    const bool decoded_tokens = lzss_ac_decode_stream(
        &decoder_codec,
        &decoder,
        &reader,
        &decoded_stream
    );

    lzss_ac_codec_destroy(&decoder_codec);

    ByteBuffer decoded_bytes;
    buffer_init(&decoded_bytes);
    buffer_init_with_capacity(&decoded_bytes, input.size());

    const bool decoded_lzss =
        decoded_tokens && lzss_decode(&decoded_stream, &decoded_bytes);
    const bool same_size = decoded_bytes.size == input.size();
    const bool same_data = same_size &&
        std::equal(input.begin(), input.end(), decoded_bytes.data);

    if (!(decoded_tokens && decoded_lzss && same_data)) {
        err << "  arithmetic decoded_tokens=" << decoded_tokens
            << " decoded_lzss=" << decoded_lzss
            << " decoded_size=" << decoded_bytes.size
            << " used_words=" << used_words << '\n';
    }

    buffer_free(&decoded_bytes);
    token_stream_free(&decoded_stream);
    return decoded_tokens && decoded_lzss && same_data;
}

struct CompressionResult {
    bool ok;
    size_t token_count;
    size_t compressed_size;
    double compress_ms;
    double decompress_ms;
};

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

static CompressionResult compress_decompress_file(
    const std::vector<uint8_t>& input,
    const LzssConfig& config,
    std::ostream& err)
{
    CompressionResult result{};

    LzssTokenStream stream;
    token_stream_init(&stream, 0);

    LzssArithmeticCodec encoder_codec{};
    LzssArithmeticCodec decoder_codec{};

    const auto compress_start = std::chrono::steady_clock::now();

    bool ok = lzss_encode(input.data(), input.size(), &config, &stream);
    if (ok) {
        ok = lzss_ac_codec_init(&encoder_codec, &config);
    }

    const size_t word_count = std::max<size_t>(
        1024,
        stream.count * 8 + input.size() + 64
    );
    std::vector<uint32_t> compressed(word_count, 0);

    ac encoder{};
    bio writer{};

    if (ok) {
        bio_open(
            &writer,
            compressed.data(),
            compressed.data() + compressed.size(),
            BIO_MODE_WRITE
        );

        ok = lzss_ac_encode_stream(
            &encoder_codec,
            &encoder,
            &writer,
            &stream
        );
        bio_close(&writer, BIO_MODE_WRITE);
    }

    const auto compress_end = std::chrono::steady_clock::now();

    if (encoder_codec.event_model.table != nullptr) {
        lzss_ac_codec_destroy(&encoder_codec);
    }

    result.token_count = stream.count;
    result.compressed_size =
        static_cast<size_t>(writer.ptr - compressed.data()) *
        sizeof(uint32_t);
    result.compress_ms =
        std::chrono::duration<double, std::milli>(
            compress_end - compress_start
        ).count();

    if (!ok) {
        err << "  compression failed\n";
        token_stream_free(&stream);
        return result;
    }

    const auto decompress_start = std::chrono::steady_clock::now();

    ok = lzss_ac_codec_init(&decoder_codec, &config);

    LzssTokenStream decoded_stream;
    token_stream_init(&decoded_stream, 0);

    if (ok) {
        ac decoder{};
        bio reader{};
        bio_open(
            &reader,
            compressed.data(),
            compressed.data() + result.compressed_size / sizeof(uint32_t),
            BIO_MODE_READ
        );

        ok = lzss_ac_decode_stream(
            &decoder_codec,
            &decoder,
            &reader,
            &decoded_stream
        );
    }

    ByteBuffer decoded_bytes;
    buffer_init(&decoded_bytes);
    buffer_init_with_capacity(&decoded_bytes, input.size());

    if (ok) {
        ok = lzss_decode(&decoded_stream, &decoded_bytes);
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
        err << "  decompression failed or data mismatch"
            << " decoded_size=" << decoded_bytes.size << '\n';
    }

    result.ok = ok && same_data;

    buffer_free(&decoded_bytes);
    token_stream_free(&decoded_stream);
    if (decoder_codec.event_model.table != nullptr) {
        lzss_ac_codec_destroy(&decoder_codec);
    }
    token_stream_free(&stream);

    return result;
}

static bool run_case(const std::string& name, const std::vector<uint8_t>& input,
                     const LzssConfig& config, std::ostream& out,
                     std::ostream& err)
{
    LzssTokenStream stream;
    token_stream_init(&stream, 0);

    ByteBuffer decoded;
    buffer_init(&decoded);
    buffer_init_with_capacity(&decoded, input.size());

    const bool encoded = lzss_encode(input.data(), input.size(), &config, &stream);
    const bool decoded_ok = encoded && lzss_decode(&stream, &decoded);
    const bool same_size = decoded.size == input.size();
    const bool same_data = same_size &&
        std::equal(input.begin(), input.end(), decoded.data);
    const bool arithmetic_ok = encoded &&
        run_arithmetic_roundtrip(stream, input, config, err);

    out << name << ": " << input.size() << " bytes, "
        << stream.count << " tokens -> "
        << (encoded && decoded_ok && same_data ? "OK" : "FAIL")
        << ", ac -> " << (arithmetic_ok ? "OK" : "FAIL") << '\n';

    if (!(encoded && decoded_ok && same_data && arithmetic_ok)) {
        err << "  encoded=" << encoded
            << " decoded=" << decoded_ok
            << " decoded_size=" << decoded.size << '\n';
    }

    buffer_free(&decoded);
    token_stream_free(&stream);
    return encoded && decoded_ok && same_data && arithmetic_ok;
}

bool run_lzss_quickcheck(std::ostream& out, std::ostream& err)
{
    const LzssConfig config{
        4096,
        3,
        18
    };

    std::mt19937 rng(0x9f723abc);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::uniform_int_distribution<int> len_dist(0, 2048);

    std::vector<std::pair<std::string, std::vector<uint8_t>>> cases;
    cases.push_back({"empty", {}});
    cases.push_back({"single", {'A'}});
    cases.push_back({"repeated", std::vector<uint8_t>(2048, 'x')});

    std::vector<uint8_t> periodic(4096);
    const std::string pattern = "aboba";
    for (size_t i = 0; i < periodic.size(); ++i) {
        periodic[i] = static_cast<uint8_t>(pattern[i % pattern.size()]);
    }
    cases.push_back({"periodic", periodic});

    for (int i = 0; i < 64; ++i) {
        std::vector<uint8_t> data(static_cast<size_t>(len_dist(rng)));
        for (uint8_t& byte : data) {
            byte = static_cast<uint8_t>(byte_dist(rng));
        }
        cases.push_back({"random_" + std::to_string(i), data});
    }

    bool all_ok = true;
    for (const auto& test : cases) {
        all_ok = run_case(test.first, test.second, config, out, err) && all_ok;
    }

    out << (all_ok ? "OK, all tests passed\n"
                  : "FAIL, some tests failed\n");
    return all_ok;
}

bool run_silesia_benchmark(std::ostream& out, std::ostream& err)
{
    const std::filesystem::path dataset_dir = "silesia";
    const LzssConfig config{
        4096,
        3,
        18
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
    out << "Compression factor = original_size / compressed_size\n\n";
    out << std::left << std::setw(14) << "file"
        << std::right << std::setw(13) << "input"
        << std::setw(13) << "compressed"
        << std::setw(11) << "factor"
        << std::setw(14) << "comp ms"
        << std::setw(14) << "decomp ms"
        << std::setw(12) << "tokens"
        << '\n';

    bool all_ok = true;
    size_t total_input = 0;
    size_t total_compressed = 0;
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

        out << std::left << std::setw(14) << path.filename().string()
            << std::right << std::setw(13) << input.size()
            << std::setw(13) << result.compressed_size
            << std::setw(11) << std::fixed << std::setprecision(3) << factor
            << std::setw(14) << std::fixed << std::setprecision(2)
            << result.compress_ms
            << std::setw(14) << std::fixed << std::setprecision(2)
            << result.decompress_ms
            << std::setw(12) << result.token_count
            << (result.ok ? "" : "  FAIL")
            << '\n';

        all_ok = result.ok && all_ok;
        total_input += input.size();
        total_compressed += result.compressed_size;
        total_compress_ms += result.compress_ms;
        total_decompress_ms += result.decompress_ms;
    }

    const double total_factor = total_compressed == 0
        ? 0.0
        : static_cast<double>(total_input) /
          static_cast<double>(total_compressed);

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
        << '\n';

    out << (all_ok ? "OK, all checks passed\n"
                  : "FAIL, some checks failed\n");
    return all_ok;
}

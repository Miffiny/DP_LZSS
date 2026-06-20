//TODO make better tests, base generated tests are implemented
#include "lzss_test.h"

#include "LZSS/decoder.h"
#include "LZSS/parser.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <utility>
#include <vector>

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

    out << name << ": " << input.size() << " bytes, "
        << stream.count << " tokens -> "
        << (encoded && decoded_ok && same_data ? "OK" : "FAIL") << '\n';

    if (!(encoded && decoded_ok && same_data)) {
        err << "  encoded=" << encoded
            << " decoded=" << decoded_ok
            << " decoded_size=" << decoded.size << '\n';
    }

    buffer_free(&decoded);
    token_stream_free(&stream);
    return encoded && decoded_ok && same_data;
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
    for (size_t i = 0; i < periodic.size(); ++i) {
        periodic[i] = static_cast<uint8_t>("aboba"[i % 11]);
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

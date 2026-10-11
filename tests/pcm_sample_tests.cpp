#include "pcm_sample.h"
#include <array>
#include <cmath>
#include <cstdio>

int main() {
    unsigned failures = 0;
    auto check = [&](std::array<std::uint8_t, 4> bytes, unsigned bits, float expected) {
        const float actual = DecodeSignedPcm(bytes.data(), bits);
        if (std::fabs(actual - expected) > 1e-7f) {
            std::fprintf(stderr, "%u-bit PCM: got %.9f expected %.9f\n", bits, actual, expected);
            ++failures;
        }
    };
    check({0, 0, 0, 0}, 16, 0);
    check({0, 0x80, 0, 0}, 16, -1);
    check({0xFF, 0x7F, 0, 0}, 16, 32767.0f / 32768.0f);
    check({0xFF, 0xFF, 0, 0}, 16, -1.0f / 32768.0f);
    check({0, 0, 0x80, 0xFF}, 24, -1);
    check({0xFF, 0xFF, 0x7F, 0xA5}, 24, 8388607.0f / 8388608.0f);
    check({0xFF, 0xFF, 0xFF, 0}, 24, -1.0f / 8388608.0f);
    check({0, 0, 0, 0x80}, 32, -1);
    check({0xFF, 0xFF, 0xFF, 0x7F}, 32, 1);
    check({0, 0, 0, 0x40}, 32, 0.5f);
    check({0, 0, 0, 0xC0}, 32, -0.5f);
    check({0, 8, 0, 0}, 12, -1);
    return failures ? 1 : 0;
}

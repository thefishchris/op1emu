#include "cpu/nand_ecc.h"
#include <array>
#include <cstdio>

int main() {
    std::array<std::uint8_t, 256> data{};
    unsigned failures = 0;
    auto check = [&](const char* name, std::uint16_t expectedOdd, std::uint16_t expectedEven) {
        auto [odd, even] = ComputeEccPair(data.data(), 256, 0);
        if (odd != expectedOdd || even != expectedEven) {
            std::fprintf(stderr, "%s: got %03X/%03X expected %03X/%03X\n", name,
                         static_cast<unsigned>(odd), static_cast<unsigned>(even),
                         static_cast<unsigned>(expectedOdd), static_cast<unsigned>(expectedEven));
            ++failures;
        }
    };
    check("all zero", 0, 0);
    data.fill(0xFF);
    check("erased sector", 0, 0);
    data.fill(0);
    data[0] = 0x01;
    check("even bit in byte zero", 0, 0x7FF);
    data[0] = 0x04;
    check("odd P2 bit in byte zero", 2, 0x7FD);
    data[0] = 0x05;
    // Two bits cancel byte parity, but each P2 complement must be set.
    // The original 'else p2 ^= bit' typo incorrectly returns 0/0 here.
    check("P2 complementary parity regression", 2, 2);
    if (ComputeEccPair(nullptr, 0, 0) != std::make_tuple(std::uint16_t{0}, std::uint16_t{0})) {
        std::fprintf(stderr, "empty transfer has nonzero ECC\n");
        ++failures;
    }
    data[128] = 0x01;
    data[255] = 0x80;
    auto whole = ComputeEccPair(data.data(), 256, 0);
    auto [a, b] = ComputeEccPair(data.data(), 128, 0);
    auto [c, d] = ComputeEccPair(data.data() + 128, 128, 128);
    if (whole != std::make_tuple(static_cast<std::uint16_t>(a ^ c), static_cast<std::uint16_t>(b ^ d))) {
        std::fprintf(stderr, "chunked ECC differs from whole-sector ECC\n");
        ++failures;
    }
    return failures == 0 ? 0 : 1;
}

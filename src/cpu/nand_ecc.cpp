#include "nand_ecc.h"

std::tuple<std::uint16_t, std::uint16_t> ComputeEccPair(
    const std::uint8_t* data, std::uint32_t length, std::uint32_t bytePos) {
    std::uint8_t p1 = 0, p1p = 0, p2 = 0, p2p = 0, p4 = 0, p4p = 0;
    std::uint8_t lp[8] = {0};
    std::uint8_t lpp[8] = {0};
    for (std::uint32_t i = 0; i < length; i++) {
        std::uint8_t d = data[i];
        std::uint8_t byteParity = 0;
        for (int pos = 0; pos < 8; pos++) {
            std::uint8_t bit = (d >> pos) & 1;
            if (pos & 0x1) p1 ^= bit;
            else           p1p ^= bit;
            if (pos & 0x2) p2 ^= bit;
            else           p2p ^= bit;
            if (pos & 0x4) p4 ^= bit;
            else           p4p ^= bit;
            byteParity ^= bit;
        }

        std::uint32_t currentBytePos = bytePos + i;
        for (int pos = 0; pos < 8; pos++) {
            if (currentBytePos & (1U << pos)) lp[pos] ^= byteParity;
            else lpp[pos] ^= byteParity;
        }
    }
    auto ecc1 = static_cast<std::uint16_t>((p1 << 0) | (p2 << 1) | (p4 << 2) | (lp[0] << 3) | (lp[1] << 4) | (lp[2] << 5) | (lp[3] << 6) | (lp[4] << 7) | (lp[5] << 8) | (lp[6] << 9) | (lp[7] << 10));
    auto ecc2 = static_cast<std::uint16_t>((p1p << 0) | (p2p << 1) | (p4p << 2) | (lpp[0] << 3) | (lpp[1] << 4) | (lpp[2] << 5) | (lpp[3] << 6) | (lpp[4] << 7) | (lpp[5] << 8) | (lpp[6] << 9) | (lpp[7] << 10));
    return {ecc1, ecc2};
}

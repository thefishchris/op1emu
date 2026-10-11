#pragma once

#include <cstdint>

// Signed little-endian PCM, carried in 16-bit or 32-bit DMA words.
// Callers validate bit widths in [1, 32] before decoding.
inline float DecodeSignedPcm(const std::uint8_t* bytes, unsigned bits) {
    std::uint32_t value = std::uint32_t{bytes[0]} | (std::uint32_t{bytes[1]} << 8);
    if (bits > 16) value |= (std::uint32_t{bytes[2]} << 16) | (std::uint32_t{bytes[3]} << 24);
    if (bits < 32) value &= (std::uint32_t{1} << bits) - 1;
    std::int64_t signedValue = value;
    if (value & (std::uint32_t{1} << (bits - 1))) signedValue -= std::int64_t{1} << bits;
    return static_cast<float>(signedValue) / static_cast<float>(std::uint64_t{1} << (bits - 1));
}

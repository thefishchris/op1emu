#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// SHA-256 (FIPS 180-4) of a byte buffer, as 64 lowercase hex digits.
std::string Sha256Hex(const std::uint8_t* data, std::size_t length);

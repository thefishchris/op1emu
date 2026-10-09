#pragma once

#include <cstdint>
#include <tuple>

std::tuple<std::uint16_t, std::uint16_t> ComputeEccPair(
    const std::uint8_t* data, std::uint32_t length, std::uint32_t bytePos);

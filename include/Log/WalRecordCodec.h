#pragma once

#include <Log/WalRecord.h>

#include <cstddef>
#include <span>
#include <vector>

namespace WalRecordCodec {
    inline constexpr std::size_t HEADER_SIZE = 40;
    inline constexpr std::uint16_t FORMAT_VERSION = 1;

    std::vector<char> encode(const WalRecord& record);
    WalRecord decode(std::span<const char> encoded);
}

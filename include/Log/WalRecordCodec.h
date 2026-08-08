#pragma once

#include <Log/WalRecord.h>

#include <cstddef>
#include <span>
#include <vector>

namespace WalRecordCodec {
    /// Width of the absolute-LSN prefix in every encoded record.
    inline constexpr std::size_t LSN_SIZE = sizeof(std::uint64_t);

    /// Encodes an absolute LSN in big-endian order followed by opaque data.
    /// Throws `std::invalid_argument` because LSN zero is reserved for "none."
    std::vector<char> encode(const WalRecord &record);

    /// Decodes one Store payload into a minimal WAL record.
    /// Throws `std::runtime_error` for a short encoding or reserved LSN zero.
    WalRecord decode(std::span<const char> encoded);
}

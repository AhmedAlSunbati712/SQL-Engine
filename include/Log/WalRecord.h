#pragma once

#include <cstdint>
#include <vector>

/// Minimal self-identifying record stored in the local write-ahead log.
///
/// `lsn` is the absolute, node-local WAL sequence number. `data` remains an
/// opaque byte payload until transaction and page-mutation record families are
/// introduced. Empty payloads are valid.
struct WalRecord {
    std::uint64_t lsn = 0;
    std::vector<char> data;
};

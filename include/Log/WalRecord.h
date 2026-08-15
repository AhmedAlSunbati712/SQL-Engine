#pragma once

#include <cstdint>
#include <vector>

using Lsn = std::uint64_t;

enum class WalRecordType : std::uint16_t {
    TxnBegin = 1,
    BTreeAction = 2,
    Compensation = 3,
    SystemAction = 4,
    TxnCommit = 5,
    TxnAbort = 6,
    TxnEnd = 7,
};

struct PendingWalRecord {
    WalRecordType type = WalRecordType::SystemAction;
    std::uint64_t transaction_id = 0;
    Lsn prev_lsn = 0;
    std::vector<char> data;
};

struct WalRecord {
    Lsn lsn = 0;
    WalRecordType type = WalRecordType::SystemAction;
    std::uint64_t transaction_id = 0;
    Lsn prev_lsn = 0;
    std::vector<char> data;
};

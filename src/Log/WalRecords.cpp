#include <Log/WalRecords.h>
#include <Log/WalPayloadCodec.h>

namespace {
PendingWalRecord make(WalRecordType type, std::uint64_t txn, Lsn prev, const WalPayload& payload) {
    return {.type = type, .transaction_id = txn, .prev_lsn = prev,
            .data = WalPayloadCodec::encode(type, payload)};
}
}

namespace WalRecords {
PendingWalRecord begin(std::uint64_t txn) { return make(WalRecordType::TxnBegin, txn, 0, BeginPayload{}); }
PendingWalRecord btree_action(std::uint64_t txn, Lsn prev, const BTreeActionPayload& p) { return make(WalRecordType::BTreeAction, txn, prev, p); }
PendingWalRecord compensation(std::uint64_t txn, Lsn prev, const CompensationPayload& p) { return make(WalRecordType::Compensation, txn, prev, p); }
PendingWalRecord system_action(const SystemActionPayload& p) { return make(WalRecordType::SystemAction, 0, 0, p); }
PendingWalRecord commit(std::uint64_t txn, Lsn prev) { return make(WalRecordType::TxnCommit, txn, prev, CommitPayload{}); }
PendingWalRecord abort(std::uint64_t txn, Lsn prev, AbortReason reason) { return make(WalRecordType::TxnAbort, txn, prev, AbortPayload{reason}); }
PendingWalRecord end(std::uint64_t txn, Lsn prev) { return make(WalRecordType::TxnEnd, txn, prev, EndPayload{}); }
WalPayload decode(const WalRecord& record) { return WalPayloadCodec::decode(record.type, record.data); }
}

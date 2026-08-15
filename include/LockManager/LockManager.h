#pragma once
#include <cstddef>
#include <array>
#include <LockShard.h>
#include <cstdint>
#include <TransactionManager/Transaction.h>
#include <Key.h>
static constexpr std::size_t SHARD_COUNT = 64;
enum class LockManagerStatus : std::uint8_t {
    Success = 0,
    TxnHoldsExclusive,
    TxnHoldsShared,
    Busy,
    NotOwner
};

class LockManager {
    public:
        explicit LockManager() = default;
        ~LockManager() = default;

        LockManager(const LockManager&) = delete;
        LockManager& operator=(const LockManager&) = delete;

        LockManager(LockManager&&) = delete;
        LockManager& operator=(LockManager&&) = delete;

        LockManagerStatus lock_shared(TransactionId txn_id, const Key& key);
        LockManagerStatus lock_exclusive(TransactionId txn_id, const Key& key);

        LockManagerStatus unlock_shared(TransactionId txn_id, const Key& key);
        LockManagerStatus unlock_exclusive(TransactionId txn_id, const Key& key);

    private:
        std::array<LockShard, SHARD_COUNT> shards;
};

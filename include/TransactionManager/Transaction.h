#pragma once

#include <Key.h>
#include <LockManager/LockMode.h>
#include <Log/WalRecord.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

using TransactionId = std::uint64_t;

enum class TransactionState : std::uint8_t {
    Active = 0,
    AbortRequested,
    Committing,
    Aborting,
    Committed,
    Aborted,
};

struct HeldKeyLock {
    // The transaction owns the encoded key for the complete lock lifetime.
    // A reference to a command-local Key could dangle before commit or abort.
    Key key;
    LockMode mode;
};

class Transaction {
public:
    explicit Transaction(TransactionId id) : id_(id) {
        if (id_ == 0) {
            throw std::invalid_argument("Transaction ID zero is reserved for none");
        }
    }

    ~Transaction() = default;

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) = delete;
    Transaction& operator=(Transaction&&) = delete;

    TransactionId id() const noexcept { return id_; }
    TransactionState state() const noexcept { return state_; }

private:
    friend class TransactionManager;

    // TransactionManager owns lifecycle transitions and metadata updates so
    // commit, abort, lock cleanup, and WAL chaining remain coordinated.
    const TransactionId id_;
    TransactionState state_ = TransactionState::Active;

    std::vector<HeldKeyLock> held_key_locks_;
    std::optional<Key> waiting_for_key_;

    Lsn last_lsn_ = 0;
};

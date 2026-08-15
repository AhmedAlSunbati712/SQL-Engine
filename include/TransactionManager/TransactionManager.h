#pragma once

#include <Log/WalPayload.h>
#include <TransactionManager/Transaction.h>
#include <TransactionManager/WaitForGraph.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

class LockManager;
class Log;

using TransactionHandle = std::shared_ptr<Transaction>;

enum class CommitStatus : std::uint8_t {
    Success = 0,
    TransactionNotFound,
    InvalidState,
};

enum class AbortStatus : std::uint8_t {
    Success = 0,
    TransactionNotFound,
    InvalidState,
};

enum class WaitRegistrationStatus : std::uint8_t {
    Registered = 0,
    Deadlock,
    TransactionNotFound,
    BlockerNotFound,
};

// Executes one logical inverse against the current B+ tree and returns every
// physical page effect produced by that inverse. TransactionManager uses the
// returned effects to append the corresponding compensation record.
class TransactionUndoExecutor {
public:
    virtual ~TransactionUndoExecutor() = default;

    virtual std::vector<PageEffect> undo(
        Transaction& transaction,
        const UndoDescriptor& undo) = 0;
};

class TransactionManager {
public:
    TransactionManager(
        Log& log,
        LockManager& lock_manager,
        TransactionUndoExecutor& undo_executor);

    ~TransactionManager() = default;

    TransactionManager(const TransactionManager&) = delete;
    TransactionManager& operator=(const TransactionManager&) = delete;
    TransactionManager(TransactionManager&&) = delete;
    TransactionManager& operator=(TransactionManager&&) = delete;

    // Creates an active transaction, appends its begin record, and returns a
    // shared handle owned by both the manager and the calling session.
    TransactionHandle begin();

    // Returns an empty handle when the transaction is not active.
    TransactionHandle find(TransactionId txn_id) const;

    // Commit and abort own the complete lifecycle boundary. I/O and WAL
    // corruption failures continue to use the logger's exception convention.
    CommitStatus commit(const TransactionHandle& transaction);
    AbortStatus abort(
        const TransactionHandle& transaction,
        AbortReason reason);

    // Registers one complete blocker set through WaitForGraph::add_edges().
    WaitRegistrationStatus register_wait(
        TransactionId waiting_txn,
        std::span<const TransactionId> blockers);

    // Removes all outgoing dependencies for a granted or cancelled request.
    void remove_wait(TransactionId waiting_txn);

private:
    Log& log_;
    LockManager& lock_manager_;
    TransactionUndoExecutor& undo_executor_;

    mutable std::mutex transactions_mutex_;
    std::unordered_map<TransactionId, TransactionHandle> active_transactions_;
    TransactionId next_transaction_id_ = 1;

    WaitForGraph wait_for_graph_;

    bool owns_handle(const TransactionHandle& transaction) const;
    void remove_transaction(TransactionId txn_id);
};

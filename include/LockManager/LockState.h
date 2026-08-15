#pragma once

#include <unordered_set>
#include <Transaction.h>
#include <optional>
#include <queue>
#include <Waiter.h>
#include <mutex>
#include <condition_variable>

struct LockState {
    std::unordered_set<TransactionId> shared_owners;
    std::optional<TransactionId> exclusive_owner;

    std::queue<Waiter> waiters;
    std::mutex mutex_;
    std::condition_variable cv_;

    static bool grant_waiters(LockState& state);
};
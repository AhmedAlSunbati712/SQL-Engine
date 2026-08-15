#pragma once

#include <unordered_set>
#include <TransactionManager/Transaction.h>
#include <optional>
#include <queue>
#include <Waiter.h>
#include <mutex>
#include <condition_variable>
#include <memory>

struct LockState {
    std::unordered_set<TransactionId> shared_owners;
    std::optional<TransactionId> exclusive_owner;

    std::queue<std::shared_ptr<Waiter>> waiters;
    std::condition_variable cv_;

    static bool grant_waiters(LockState& state) {
        if (state.exclusive_owner || state.waiters.empty()) {
            return false;
        }
        // While the waiters queue is not empty, pop one waiter by one
        std::queue<std::shared_ptr<Waiter>>& queue = state.waiters;
        bool granted = false; // Flag to track if we granted at least one waiter

        while (!queue.empty()) {
            // Peek at the front of the queue
            std::shared_ptr<Waiter> waiter = queue.front();

            // If the front waiter wants exclusive
            if (waiter->lock_mode == LockMode::Exclusive) {

                // Check first if there are no shared owners
                if (state.shared_owners.empty()) {
                    queue.pop(); // Remove it from the queue
                    state.exclusive_owner = waiter->txn_id; // set the exclusive owner on this key lock state
                    waiter->granted = true; // Grant it
                    return true; // Return
                }
                break; // Otherwise break. The queue is blocked
            }

            // Otherwise, this is a shared lock request
            queue.pop();
            state.shared_owners.insert(waiter->txn_id);
            waiter->granted = true; // Grant it
            granted = true; // Set the flag to true
        }
        return granted;
    }
};

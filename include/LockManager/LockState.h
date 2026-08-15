#pragma once

#include <unordered_set>
#include <TransactionManager/Transaction.h>
#include <optional>
#include <deque>
#include <Waiter.h>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <vector>

struct LockState {
    std::unordered_set<TransactionId> shared_owners;
    std::optional<TransactionId> exclusive_owner;

    std::deque<std::shared_ptr<Waiter>> waiters;
    std::condition_variable cv_;

    static std::vector<std::shared_ptr<Waiter>> grant_waiters(LockState& state) {
        if (state.exclusive_owner || state.waiters.empty()) {
            return {};
        }
        // While the waiters queue is not empty, pop one waiter by one
        std::deque<std::shared_ptr<Waiter>>& queue = state.waiters;
        std::vector<std::shared_ptr<Waiter>> granted;
        granted.reserve(queue.size());

        while (!queue.empty()) {
            // Peek at the front of the queue
            std::shared_ptr<Waiter> waiter = queue.front();

            // If the front waiter wants exclusive
            if (waiter->lock_mode == LockMode::Exclusive) {

                // A promoting waiter may replace its own retained shared lock
                // after every other shared owner has released theirs.
                const bool only_requester_is_shared =
                    state.shared_owners.size() == 1 &&
                    state.shared_owners.contains(waiter->txn_id);
                if (state.shared_owners.empty() || only_requester_is_shared) {
                    queue.pop_front(); // Remove it from the queue
                    if (only_requester_is_shared) {
                        state.shared_owners.erase(waiter->txn_id);
                    }
                    state.exclusive_owner = waiter->txn_id; // set the exclusive owner on this key lock state
                    waiter->granted = true; // Grant it
                    granted.push_back(waiter);
                    return granted; // Return
                }
                break; // Otherwise break. The queue is blocked
            }

            // Otherwise, this is a shared lock request
            queue.pop_front();
            state.shared_owners.insert(waiter->txn_id);
            waiter->granted = true; // Grant it
            granted.push_back(waiter);
        }
        return granted;
    }
};

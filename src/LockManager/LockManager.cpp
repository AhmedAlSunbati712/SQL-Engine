#include <LockManager.h>
#include <mutex>
#include <Waiter.h>
#include <LockMode.h>
#include <TransactionManager.h>
#include <exception>
#include <stdexcept>

// Currently handles:
// NoLock to Shared Lock
// Shared stays shared
// Exclusive stays exclusive
LockManagerStatus LockManager::lock_shared(TransactionId txn_id, const Key& key) {
    std::size_t _hash = KeyHash{}(key);
    LockShard &shard = shards[_hash % SHARD_COUNT];

    // Need to acquire a lock on the shard first
    std::unique_lock lock(shard.mutex_);

    // Then find the lock state corresponding to this key
    LockState& state = shard.states[key];

    // First check if the lock is one of the shared owners already
    // Callers shouldn't add another lock to the txn held locks
    auto it = state.shared_owners.find(txn_id);
    if (it != state.shared_owners.end()) {
        return LockManagerStatus::TxnHoldsShared;
    }

    // Check if it's an exclusive owner
    if (state.exclusive_owner && *state.exclusive_owner == txn_id) {
        return LockManagerStatus::TxnHoldsExclusive;
    }

    // We can grant lock access immediately without waiting only if there's
    // no exclusive owners and waiters is empty
    if (!state.exclusive_owner && state.waiters.empty()) {
        // Add this transaction id to the shared owners
        state.shared_owners.insert(txn_id);

        // Track transaction-owned locks only when this manager is attached to
        // the transaction subsystem. Standalone lock-table use remains valid.
        try {
            if (txn_mgr_ && !txn_mgr_->record_lock(txn_id, key, LockMode::Shared)) {
                state.shared_owners.erase(txn_id);
                shard.states.erase(key);
                return LockManagerStatus::TransactionNotFound;
            }
        } catch (...) {
            state.shared_owners.erase(txn_id);
            shard.states.erase(key);
            throw;
        }
        return LockManagerStatus::Success;
    }

    // Otherwise, we will have to wait
    // Determine the blockers first so we can add them in the waitfor graph
    std::vector<TransactionId> blockers{};
    if (state.exclusive_owner) blockers.push_back(*state.exclusive_owner);
    for (std::shared_ptr<Waiter> waiter : state.waiters) {
        if (waiter->lock_mode == LockMode::Exclusive) {
            blockers.push_back(waiter->txn_id);
        }
    }
    std::shared_ptr<Waiter> waiter = std::make_shared<Waiter>();
    waiter->txn_id = txn_id;
    waiter->lock_mode = LockMode::Shared;

    // Push it to the queue of waiters
    state.waiters.push_back(waiter);

    // Register the graph edges only after the waiter is safely in the queue.
    // The shard mutex prevents owners or earlier waiters from changing here.
    if (txn_mgr_) {
        WaitRegistrationStatus registration_status;
        try {
            registration_status = txn_mgr_->register_wait(txn_id, blockers);
        } catch (...) {
            state.waiters.pop_back();
            throw;
        }
        if (registration_status != WaitRegistrationStatus::Registered) {
            state.waiters.pop_back();
            if (registration_status == WaitRegistrationStatus::Deadlock) {
                return LockManagerStatus::Deadlock;
            }
            return LockManagerStatus::TransactionNotFound;
        }
    }

    // Wait until someone grants us the lock on the key
    state.cv_.wait(lock, [&]{
        return waiter->granted || waiter->cancelled;
    });

    return waiter->cancelled
        ? LockManagerStatus::TransactionNotFound
        : LockManagerStatus::Success;
}

// Currently handles:
// NoLock to Exclusive Lock
// Shared stays shared
// Exclusive stays exclusive
LockManagerStatus LockManager::lock_exclusive(TransactionId txn_id, const Key& key) {
    std::size_t _hash = KeyHash{}(key);
    LockShard &shard = shards[_hash % SHARD_COUNT];

    // Need to acquire a lock on the shard first
    std::unique_lock lock(shard.mutex_);

    // Then find the lock state corresponding to this key
    LockState& state = shard.states[key];

    // First check if it's the exclusive owner already
    // Callers shouldn't add another lock to the txn held locks
    if (state.exclusive_owner && *state.exclusive_owner == txn_id) {
        return LockManagerStatus::TxnHoldsExclusive;
    }

    // Check whether this transaction is promoting its existing shared lock.
    // It remains a shared owner while waiting for every other reader.
    auto it = state.shared_owners.find(txn_id);
    const bool is_promotion = it != state.shared_owners.end();

    // We can grant lock access immediately without waiting only if there's
    // no other shared owner, no exclusive owner, and no earlier waiter
    const bool no_other_shared_owner = state.shared_owners.empty() ||
        (is_promotion && state.shared_owners.size() == 1);
    if (no_other_shared_owner && !state.exclusive_owner && state.waiters.empty()) {
        // Promotion replaces this transaction's shared ownership atomically.
        if (is_promotion) state.shared_owners.erase(txn_id);

        // Set this transaction id as the exclusive owner
        state.exclusive_owner = txn_id;

        try {
            if (txn_mgr_ && !txn_mgr_->record_lock(txn_id, key, LockMode::Exclusive)) {
                state.exclusive_owner.reset();
                if (is_promotion) {
                    state.shared_owners.insert(txn_id);
                } else {
                    shard.states.erase(key);
                }
                return LockManagerStatus::TransactionNotFound;
            }
        } catch (...) {
            state.exclusive_owner.reset();
            if (is_promotion) {
                state.shared_owners.insert(txn_id);
            } else {
                shard.states.erase(key);
            }
            throw;
        }
        return LockManagerStatus::Success;
    }

    // An exclusive request waits for every current owner and every earlier
    // waiter because it cannot pass any request already in the FIFO queue.
    std::vector<TransactionId> blockers{};
    if (state.exclusive_owner) blockers.push_back(*state.exclusive_owner);
    for (TransactionId shared_owner : state.shared_owners) {
        // A promotion must not wait for its own retained shared ownership.
        if (shared_owner != txn_id) blockers.push_back(shared_owner);
    }
    for (const std::shared_ptr<Waiter>& queued : state.waiters) {
        blockers.push_back(queued->txn_id);
    }

    // Otherwise, create a waiter and add it to the waiters queue
    std::shared_ptr<Waiter> waiter = std::make_shared<Waiter>();
    waiter->txn_id = txn_id;
    waiter->lock_mode = LockMode::Exclusive;

    // Push it to the queue of waiters
    state.waiters.push_back(waiter);

    if (txn_mgr_) {
        WaitRegistrationStatus registration_status;
        try {
            registration_status = txn_mgr_->register_wait(txn_id, blockers);
        } catch (...) {
            state.waiters.pop_back();
            throw;
        }
        if (registration_status != WaitRegistrationStatus::Registered) {
            state.waiters.pop_back();
            if (registration_status == WaitRegistrationStatus::Deadlock) {
                return LockManagerStatus::Deadlock;
            }
            return LockManagerStatus::TransactionNotFound;
        }
    }

    // Wait until someone grants us the lock on the key
    state.cv_.wait(lock, [&]{
        return waiter->granted || waiter->cancelled;
    });

    return waiter->cancelled
        ? LockManagerStatus::TransactionNotFound
        : LockManagerStatus::Success;
}

LockManagerStatus LockManager::unlock_shared(TransactionId txn_id, const Key& key) {
    std::size_t hash = KeyHash{}(key);
    LockShard& shard = shards[hash % SHARD_COUNT];

    // Need to acquire a lock on the shard first
    std::unique_lock lock(shard.mutex_);

    // Then find the lock state corresponding to this key
    auto state_it = shard.states.find(key);
    if (state_it == shard.states.end()) {
        return LockManagerStatus::NotOwner;
    }
    LockState& state = state_it->second;

    // Try to erase it from the shared owners
    // if it doesn't exist, this transaction isn't an owner
    if (state.shared_owners.erase(txn_id) == 0) {
        return LockManagerStatus::NotOwner;
    }

    // Then try to grant eligible waiters their requested locks
    // if there was at least one eligible waiter, this will return true
    const bool granted = grant_waiters(state, key);

    // If there's an eligible waiter, notify all threads to wake up
    // those that werent granted will go back to sleep zzzz
    if (granted) {
        state.cv_.notify_all();
    }

    // Finally, if there are no shared owners, no exclusive owner
    // and no waiters, delete this state from the shard
    if (!state.exclusive_owner && state.waiters.empty() && state.shared_owners.empty()) {
        shard.states.erase(state_it);
    }

    return LockManagerStatus::Success;
}

LockManagerStatus LockManager::unlock_exclusive(TransactionId txn_id, const Key& key) {
    std::size_t hash = KeyHash{}(key);
    LockShard& shard = shards[hash % SHARD_COUNT];

    // Need to acquire a lock on the shard first
    std::unique_lock lock(shard.mutex_);

    // Then find the lock state corresponding to this key
    auto state_it = shard.states.find(key);
    if (state_it == shard.states.end()) {
        return LockManagerStatus::NotOwner;
    }
    LockState& state = state_it->second;

    // Check if this transaction is the exclusive owner
    // if it isn't, this transaction can't release the lock
    if (!state.exclusive_owner || *state.exclusive_owner != txn_id) {
        return LockManagerStatus::NotOwner;
    }

    // Clear this transaction as the exclusive owner
    state.exclusive_owner.reset();

    // Then try to grant eligible waiters their requested locks
    // if there was at least one eligible waiter, this will return true
    const bool granted = grant_waiters(state, key);

    // If there's an eligible waiter, notify all threads to wake up
    // those that werent granted will go back to sleep zzzz
    if (granted) {
        state.cv_.notify_all();
    }

    // Finally, if there are no shared owners, no exclusive owner
    // and no waiters, delete this state from the shard
    if (!state.exclusive_owner && state.waiters.empty() && state.shared_owners.empty()) {
        shard.states.erase(state_it);
    }

    return LockManagerStatus::Success;
}

void LockManager::attach_txn_manager(TransactionManager &txn_mgr) {
    if (txn_mgr_ && txn_mgr_ != &txn_mgr) {
        throw std::logic_error("LockManager already has a TransactionManager");
    }
    txn_mgr_ = &txn_mgr;
}

void LockManager::detach_txn_manager(TransactionManager& txn_mgr) noexcept {
    if (txn_mgr_ == &txn_mgr) txn_mgr_ = nullptr;
}

bool LockManager::grant_waiters(LockState& state, const Key& key) {
    bool completed = false;
    std::exception_ptr failure;

    for (;;) {
        std::vector<std::shared_ptr<Waiter>> granted = LockState::grant_waiters(state);
        if (granted.empty()) break;

        bool retry = false;
        for (const std::shared_ptr<Waiter>& waiter : granted) {
            completed = true;
            if (!txn_mgr_) continue;

            bool recorded = false;
            try {
                recorded = txn_mgr_->record_lock(waiter->txn_id, key, waiter->lock_mode);
            } catch (...) {
                failure = std::current_exception();
            }

            // A granted or cancelled request no longer has outgoing wait
            // dependencies. Remove them before its thread can wake up.
            txn_mgr_->remove_wait(waiter->txn_id);
            if (recorded) continue;

            if (waiter->lock_mode == LockMode::Shared) {
                state.shared_owners.erase(waiter->txn_id);
            } else if (state.exclusive_owner == waiter->txn_id) {
                state.exclusive_owner.reset();
            }
            waiter->granted = false;
            waiter->cancelled = true;
            retry = true;
        }

        if (!retry) break;
    }

    // Allocation failure while recording ownership must not leave a waiter
    // asleep after its queue entry has already been removed.
    if (failure) {
        if (completed) state.cv_.notify_all();
        std::rethrow_exception(failure);
    }
    return completed;
}

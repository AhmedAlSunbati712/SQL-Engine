#include <LockManager.h>
#include <mutex>
#include <Waiter.h>
#include <LockMode.h>

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
        return LockManagerStatus::Success;
    }

    // Otherwise, create a waiter and add it to the waiters queue
    std::shared_ptr<Waiter> waiter = std::make_shared<Waiter>();
    waiter->txn_id = txn_id;
    waiter->lock_mode = LockMode::Shared;

    // Push it to the queue of waiters
    state.waiters.push(waiter);

    // Wait until someone grants us the lock on the key
    state.cv_.wait(lock, [&]{
        return waiter->granted;
    });

    return LockManagerStatus::Success;
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

    // Check if the lock is one of the shared owners already
    // Promotion path is not supported here yet until we add transaction management
    // and wait-for graphs
    auto it = state.shared_owners.find(txn_id);
    if (it != state.shared_owners.end()) {
        return LockManagerStatus::TxnHoldsShared;
    }

    // We can grant lock access immediately without waiting only if there's
    // no shared owners, no exclusive owner, and waiters is empty
    if (state.shared_owners.empty() && !state.exclusive_owner && state.waiters.empty()) {
        // Set this transaction id as the exclusive owner
        state.exclusive_owner = txn_id;
        return LockManagerStatus::Success;
    }

    // Otherwise, create a waiter and add it to the waiters queue
    std::shared_ptr<Waiter> waiter = std::make_shared<Waiter>();
    waiter->txn_id = txn_id;
    waiter->lock_mode = LockMode::Exclusive;

    // Push it to the queue of waiters
    state.waiters.push(waiter);

    // Wait until someone grants us the lock on the key
    state.cv_.wait(lock, [&]{
        return waiter->granted;
    });

    return LockManagerStatus::Success;
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
    const bool granted = LockState::grant_waiters(state);

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

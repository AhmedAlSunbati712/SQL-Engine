#pragma once

#include <unordered_map>
#include <Key.h>
#include <KeyHash.h>
#include <mutex>
#include <LockState.h>

struct LockShard {
    std::unordered_map<Key, LockState, KeyHash, KeyEqual> states;
    std::mutex mutex_;
};

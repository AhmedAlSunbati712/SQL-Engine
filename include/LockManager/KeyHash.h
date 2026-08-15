#pragma once

#include <Key.h>

#include <cstddef>
#include <cstdint>

struct KeyHash {
    std::size_t operator()(const Key& key) const noexcept {
        std::uint64_t hash = 14695981039346656037ULL;
        constexpr std::uint64_t prime = 1099511628211ULL;

        hash ^= static_cast<std::uint8_t>(key.type);
        hash *= prime;

        for (char byte : key.data) {
            hash ^= static_cast<unsigned char>(byte);
            hash *= prime;
        }

        if constexpr (sizeof(std::size_t) < sizeof(hash)) {
            hash ^= hash >> 32;
        }

        return static_cast<std::size_t>(hash);
    }
};

struct KeyEqual {
    bool operator()(const Key& lhs, const Key& rhs) const {
        return lhs.type == rhs.type && lhs.data == rhs.data;
    }
};

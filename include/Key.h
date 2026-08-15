#pragma once

#include <cstdint>
#include <vector>
#include <KeyCodec.h>

enum class KeyType : std::uint8_t {
    Bool,
    UInt64,
    Int64,
    String,
    Bytes
};

struct Key {
    // data holds the encoded key bytes that the storage engine persists and compares.
    // size is the number of encoded bytes in data.
    // type tracks the logical key family that produced this encoded representation.
    KeyType type;
    std::uint32_t size;
    std::vector<char> data;
};


struct KeyHash {
    std::size_t operator()(const Key& key) const noexcept {
        std::uint64_t hash = 14695981039346656037ULL;
        constexpr std::uint64_t prime = 1099511628211ULL;

        hash ^= static_cast<std::uint64_t>(key.type);
        hash *= prime;

        for (char byte : key.data) {
            hash ^= static_cast<unsigned char>(byte);
            hash *= prime;
        }

        // For platforms (32-bits) where size_t is smaller than 8 bytes, the upper 4 bytes
        // of the hash get discarded. Therefore, let's xor the upper half with the lower half
        // so that we dont lose info
        // constexpr so thats evaluated in the compiled code and not during runtime
        if constexpr (sizeof(std::size_t) < sizeof(hash)) {
            hash ^= hash >> 32;
        }

        return static_cast<std::size_t>(hash);
    }
};

struct KeyEqual {
    bool operator()(const Key& lhs, const Key& rhs) const noexcept {
        return KeyCodec::equal(lhs, rhs);
    }
};
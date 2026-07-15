#pragma once

#include <cstdint>
#include <vector>

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

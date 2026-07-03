#pragma once

#include <cstdint>
#include <vector>

enum class ValueType : std::uint8_t {
    VarInt,
    Bool,
    Char
};

struct Value {
    ValueType type;
    std::uint32_t size;
    std::vector<char> data;
};

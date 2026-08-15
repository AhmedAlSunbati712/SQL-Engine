#pragma once
#include <cstdint>

enum class LockMode : std::uint8_t {
    Shared = 0,
    Exclusive,
};
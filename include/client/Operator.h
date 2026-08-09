#pragma once
#include <cstdint>

enum class OperatorType : std::uint8_t {
    NULLARY = 0,
    UNARY,
    BINARY
};

enum class Operator : std::uint8_t {
    GET = 0,
    PUT,
    DELETE,
    BEGIN_TXN,
    COMMIT,
    ROLLBACK,
};
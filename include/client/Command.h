#pragma once

#include <Key.h>
#include <Operator.h>
#include <Value.h>

#include <cstdint>
#include <optional>
#include <vector>

struct Command {
    OperatorType operator_type;
    Operator op;
    std::optional<Key> key = std::nullopt;
    std::optional<Value> value = std::nullopt;
};

namespace CommandCodec {

std::vector<std::uint8_t> serialize(const Command& command);
Command deserialize(const std::vector<std::uint8_t>& buffer);

} // namespace CommandCodec

#pragma once

#include <Log/WalPayload.h>

#include <span>
#include <vector>

namespace WalPayloadCodec {
    std::vector<char> encode(WalRecordType type, const WalPayload& payload);
    WalPayload decode(WalRecordType type, std::span<const char> payload);
}

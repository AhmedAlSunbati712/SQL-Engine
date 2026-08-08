#pragma once
#include <cstdint>
#include <vector>
#include <arpa/inet.h>
#include <cstring>
#include <sys/socket.h>
#include <string>
struct Message {
    std::uint32_t id;
    std::string text;
};

namespace MessageCodec {
    std::vector<std::uint8_t> serialize(const Message& msg);
    Message deserialize(const std::vector<std::uint8_t>& buffer);
}
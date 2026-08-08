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


std::vector<std::uint8_t> serialize(const Message& msg) {
    std::vector<std::uint8_t> buffer;

    std::uint32_t id = htonl(msg.id);
    std::uint32_t text_size = htonl(static_cast<std::uint32_t>(msg.text.size()));
    std::uint32_t payload_size = sizeof(id) + sizeof(text_size) + msg.text.size();
    std::uint32_t net_payload_size = htonl(payload_size);

    buffer.resize(sizeof(net_payload_size) + payload_size);
    size_t offset = 0;

    std::memcpy(buffer.data() + offset, &net_payload_size, sizeof(net_payload_size));
    offset += sizeof(net_payload_size);

    std::memcpy(buffer.data() + offset, &id, sizeof(id));
    offset += sizeof(id);

    std::memcpy(buffer.data() + offset, &text_size, sizeof(text_size));
    offset += sizeof(text_size);

    std::memcpy(buffer.data() + offset, msg.text.data(), msg.text.size());

    return buffer;
}

Message deserialize(const std::vector<std::uint8_t> buffer) {
    Message msg{};
    int byte_offset = 4;

    std::uint32_t network_id;
    std::memcpy(&network_id, buffer.data() + byte_offset, sizeof(network_id));
    std::uint32_t host_id = ntohl(network_id);
    msg.id = host_id;
    byte_offset += 4;


    std::uint32_t network_text_size;
    std::memcpy(&network_text_size, buffer.data() + byte_offset, sizeof(network_text_size));
    std::uint32_t host_text_size = ntohl(network_text_size);
    byte_offset += 4;

    auto start_it = buffer.data() + byte_offset;
    auto end_it = start_it + host_text_size;
    msg.text = std::string(start_it, end_it);
    return msg;
}
#include <Endian.h>

void put_u32_be(char out[4], std::uint32_t value) {
    out[0] = static_cast<char>((value >> 24) & 0xFF);
    out[1] = static_cast<char>((value >> 16) & 0xFF);
    out[2] = static_cast<char>((value >> 8) & 0xFF);
    out[3] = static_cast<char>(value & 0xFF);
}

std::uint32_t get_u32_be(char in[4]) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(in[0])) << 24 |
            static_cast<std::uint32_t>(static_cast<unsigned char>(in[1])) << 16 |
            static_cast<std::uint32_t>(static_cast<unsigned char>(in[2])) << 8  |
            static_cast<std::uint32_t>(static_cast<unsigned char>(in[3])));
}
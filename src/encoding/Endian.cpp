#include <Endian.h>

void put_u32_be(char *out, std::uint32_t value) {
    out[0] = static_cast<char>((value >> 24) & 0xFF);
    out[1] = static_cast<char>((value >> 16) & 0xFF);
    out[2] = static_cast<char>((value >> 8) & 0xFF);
    out[3] = static_cast<char>(value & 0xFF);
}

std::uint32_t get_u32_be(const char *in) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(in[0])) << 24 |
            static_cast<std::uint32_t>(static_cast<unsigned char>(in[1])) << 16 |
            static_cast<std::uint32_t>(static_cast<unsigned char>(in[2])) << 8  |
            static_cast<std::uint32_t>(static_cast<unsigned char>(in[3])));
}

void put_u64_be(char *out, std::uint64_t value) {
    out[0] = static_cast<char>((value >> 56) & 0xFF);
    out[1] = static_cast<char>((value >> 48) & 0xFF);
    out[2] = static_cast<char>((value >> 40) & 0xFF);
    out[3] = static_cast<char>((value >> 32) & 0xFF);
    out[4] = static_cast<char>((value >> 24) & 0xFF);
    out[5] = static_cast<char>((value >> 16) & 0xFF);
    out[6] = static_cast<char>((value >> 8) & 0xFF);
    out[7] = static_cast<char>(value & 0xFF);
}

std::uint64_t get_u64_be(const char *in) {
    return (static_cast<std::uint64_t>(static_cast<unsigned char>(in[0])) << 56 |
            static_cast<std::uint64_t>(static_cast<unsigned char>(in[1])) << 48 |
            static_cast<std::uint64_t>(static_cast<unsigned char>(in[2])) << 40 |
            static_cast<std::uint64_t>(static_cast<unsigned char>(in[3])) << 32 |
            static_cast<std::uint64_t>(static_cast<unsigned char>(in[4])) << 24 |
            static_cast<std::uint64_t>(static_cast<unsigned char>(in[5])) << 16 |
            static_cast<std::uint64_t>(static_cast<unsigned char>(in[6])) << 8  |
            static_cast<std::uint64_t>(static_cast<unsigned char>(in[7])));
}

void put_u16_be(char *out, std::uint16_t value) {
    out[0] = static_cast<char>((value >> 8) & 0xFF);
    out[1] = static_cast<char>(value & 0xFF);
}

std::uint16_t get_u16_be(const char *in) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(in[0])) << 8  |
            static_cast<std::uint32_t>(static_cast<unsigned char>(in[1])));
}

void put_u8_be(char *out, std::uint8_t value) {
    out[0] = static_cast<char>(value & 0xFF);
}

std::uint8_t get_u8_be(const char *in) {
    return static_cast<std::uint8_t>(static_cast<unsigned char>(in[0]));
}

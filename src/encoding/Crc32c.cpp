#include <Crc32c.h>

#include <cstdint>

std::uint32_t crc32c(std::span<const char> data) {
    std::uint32_t crc = 0xFFFFFFFFu;

    for (char value : data) {
        crc ^= static_cast<unsigned char>(value);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1u)));
        }
    }

    return crc ^ 0xFFFFFFFFu;
}

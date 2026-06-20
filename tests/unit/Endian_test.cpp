#include <gtest/gtest.h>

#include <Endian.h>

#include <array>
#include <cstdint>

namespace {

TEST(EndianTest, PutU32BeWritesBytesInBigEndianOrder) {
    std::array<char, 4> buffer{};

    put_u32_be(buffer.data(), 0x12345678);

    EXPECT_EQ(static_cast<unsigned char>(buffer[0]), 0x12);
    EXPECT_EQ(static_cast<unsigned char>(buffer[1]), 0x34);
    EXPECT_EQ(static_cast<unsigned char>(buffer[2]), 0x56);
    EXPECT_EQ(static_cast<unsigned char>(buffer[3]), 0x78);
}

TEST(EndianTest, GetU32BeReadsBytesInBigEndianOrder) {
    std::array<char, 4> buffer{
        static_cast<char>(0x12),
        static_cast<char>(0x34),
        static_cast<char>(0x56),
        static_cast<char>(0x78),
    };

    std::uint32_t value = get_u32_be(buffer.data());

    EXPECT_EQ(value, 0x12345678u);
}

TEST(EndianTest, RoundTripZeroValue) {
    std::array<char, 4> buffer{};

    put_u32_be(buffer.data(), 0u);
    std::uint32_t value = get_u32_be(buffer.data());

    EXPECT_EQ(value, 0u);
}

TEST(EndianTest, RoundTripMaxValue) {
    std::array<char, 4> buffer{};

    put_u32_be(buffer.data(), 0xFFFFFFFFu);
    std::uint32_t value = get_u32_be(buffer.data());

    EXPECT_EQ(value, 0xFFFFFFFFu);
}

TEST(EndianTest, RoundTripMixedByteValue) {
    std::array<char, 4> buffer{};

    put_u32_be(buffer.data(), 0xA1B2C3D4u);
    std::uint32_t value = get_u32_be(buffer.data());

    EXPECT_EQ(value, 0xA1B2C3D4u);
}

} // namespace

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

TEST(EndianTest, PutU16BeWritesBytesInBigEndianOrder) {
    std::array<char, 2> buffer{};

    put_u16_be(buffer.data(), 0x1234u);

    EXPECT_EQ(static_cast<unsigned char>(buffer[0]), 0x12);
    EXPECT_EQ(static_cast<unsigned char>(buffer[1]), 0x34);
}

TEST(EndianTest, GetU16BeReadsBytesInBigEndianOrder) {
    std::array<char, 2> buffer{
        static_cast<char>(0x12),
        static_cast<char>(0x34),
    };

    std::uint16_t value = get_u16_be(buffer.data());

    EXPECT_EQ(value, 0x1234u);
}

TEST(EndianTest, RoundTripU16MaxValue) {
    std::array<char, 2> buffer{};

    put_u16_be(buffer.data(), 0xFFFFu);
    std::uint16_t value = get_u16_be(buffer.data());

    EXPECT_EQ(value, 0xFFFFu);
}

TEST(EndianTest, PutU8BeWritesByte) {
    std::array<char, 1> buffer{};

    put_u8_be(buffer.data(), 0xABu);

    EXPECT_EQ(static_cast<unsigned char>(buffer[0]), 0xAB);
}

TEST(EndianTest, GetU8BeReadsByte) {
    std::array<char, 1> buffer{
        static_cast<char>(0xAB),
    };

    std::uint8_t value = get_u8_be(buffer.data());

    EXPECT_EQ(value, 0xABu);
}

TEST(EndianTest, RoundTripU8MaxValue) {
    std::array<char, 1> buffer{};

    put_u8_be(buffer.data(), 0xFFu);
    std::uint8_t value = get_u8_be(buffer.data());

    EXPECT_EQ(value, 0xFFu);
}

}

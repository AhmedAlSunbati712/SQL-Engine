#include <gtest/gtest.h>

#include <Crc32c.h>

#include <array>
#include <string_view>

TEST(Crc32cTest, MatchesStandardCheckValue) {
    constexpr std::string_view input = "123456789";
    EXPECT_EQ(crc32c(std::span<const char>{input.data(), input.size()}), 0xE3069283u);
}

TEST(Crc32cTest, HandlesBinaryInput) {
    constexpr std::array<char, 5> input{0, static_cast<char>(0xFF), 1, 0, 2};
    EXPECT_EQ(crc32c(input), 0x290B7CFEu);
}

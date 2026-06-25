#include <gtest/gtest.h>

#include <DBHeaderCodec.h>

#include <array>
#include <cstdint>

namespace {

TEST(DBHeaderCodecTest, SerializeAndDeserializeHeaderRoundTrip) {
    DBHeader header{};
    header.file_change_counter = 17u;
    header.db_page_count = 42u;
    header.freelist_head_page_num = 9u;
    header.freelist_page_count = 3u;

    std::array<char, 32> buffer{};
    DBHeaderCodec::serialize_DBHeader(header, buffer.data());

    DBHeader decoded{};
    DBHeaderCodec::deserialize_DBHeader(decoded, buffer.data());

    for (int idx = 0; idx < 16; idx++) {
        EXPECT_EQ(decoded.magic_header[idx], header.magic_header[idx]);
    }
    EXPECT_EQ(decoded.file_change_counter, 17u);
    EXPECT_EQ(decoded.db_page_count, 42u);
    EXPECT_EQ(decoded.freelist_head_page_num, 9u);
    EXPECT_EQ(decoded.freelist_page_count, 3u);
}

TEST(DBHeaderCodecTest, HeaderSerializationUsesBigEndianForIntegers) {
    DBHeader header{};
    header.file_change_counter = 0x01020304u;
    header.db_page_count = 0x11223344u;
    header.freelist_head_page_num = 0x55667788u;
    header.freelist_page_count = 0xA1B2C3D4u;

    std::array<char, 32> buffer{};
    DBHeaderCodec::serialize_DBHeader(header, buffer.data());

    EXPECT_EQ(static_cast<unsigned char>(buffer[16]), 0x01);
    EXPECT_EQ(static_cast<unsigned char>(buffer[17]), 0x02);
    EXPECT_EQ(static_cast<unsigned char>(buffer[18]), 0x03);
    EXPECT_EQ(static_cast<unsigned char>(buffer[19]), 0x04);

    EXPECT_EQ(static_cast<unsigned char>(buffer[20]), 0x11);
    EXPECT_EQ(static_cast<unsigned char>(buffer[21]), 0x22);
    EXPECT_EQ(static_cast<unsigned char>(buffer[22]), 0x33);
    EXPECT_EQ(static_cast<unsigned char>(buffer[23]), 0x44);

    EXPECT_EQ(static_cast<unsigned char>(buffer[24]), 0x55);
    EXPECT_EQ(static_cast<unsigned char>(buffer[25]), 0x66);
    EXPECT_EQ(static_cast<unsigned char>(buffer[26]), 0x77);
    EXPECT_EQ(static_cast<unsigned char>(buffer[27]), 0x88);

    EXPECT_EQ(static_cast<unsigned char>(buffer[28]), 0xA1);
    EXPECT_EQ(static_cast<unsigned char>(buffer[29]), 0xB2);
    EXPECT_EQ(static_cast<unsigned char>(buffer[30]), 0xC3);
    EXPECT_EQ(static_cast<unsigned char>(buffer[31]), 0xD4);
}

TEST(DBHeaderCodecTest, ValidateReturnsTrueForValidHeaderWithEmptyFreelist) {
    DBHeader header{};
    header.db_page_count = 8u;
    header.freelist_head_page_num = 0u;
    header.freelist_page_count = 0u;

    EXPECT_EQ(DBHeaderCodec::validate_DBHeader(header), true);
}

TEST(DBHeaderCodecTest, ValidateReturnsTrueForValidHeaderWithFreelist) {
    DBHeader header{};
    header.db_page_count = 8u;
    header.freelist_head_page_num = 5u;
    header.freelist_page_count = 2u;

    EXPECT_EQ(DBHeaderCodec::validate_DBHeader(header), true);
}

TEST(DBHeaderCodecTest, ValidateReturnsFalseForCorruptedMagic) {
    DBHeader header{};
    header.magic_header[4] = 0x00;

    EXPECT_EQ(DBHeaderCodec::validate_DBHeader(header), false);
}

TEST(DBHeaderCodecTest, ValidateReturnsFalseWhenEmptyFreelistHasNonzeroHead) {
    DBHeader header{};
    header.db_page_count = 8u;
    header.freelist_head_page_num = 3u;
    header.freelist_page_count = 0u;

    EXPECT_EQ(DBHeaderCodec::validate_DBHeader(header), false);
}

TEST(DBHeaderCodecTest, ValidateReturnsFalseWhenFreelistHasZeroHead) {
    DBHeader header{};
    header.db_page_count = 8u;
    header.freelist_head_page_num = 0u;
    header.freelist_page_count = 2u;

    EXPECT_EQ(DBHeaderCodec::validate_DBHeader(header), false);
}

TEST(DBHeaderCodecTest, ValidateReturnsFalseWhenFreelistHeadExceedsPageCount) {
    DBHeader header{};
    header.db_page_count = 8u;
    header.freelist_head_page_num = 9u;
    header.freelist_page_count = 1u;

    EXPECT_EQ(DBHeaderCodec::validate_DBHeader(header), false);
}

} // namespace

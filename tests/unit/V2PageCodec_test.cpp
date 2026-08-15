#include <gtest/gtest.h>

#include <V2PageCodec.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {

constexpr std::size_t PAGE_NUM_OFFSET = 4;
constexpr std::size_t PAGE_LSN_OFFSET = 8;
constexpr std::size_t PAGE_CHECKSUM_OFFSET = 16;
constexpr std::size_t PAGE_KIND_OFFSET = 20;

unsigned int byte_value(char value) {
    return static_cast<unsigned char>(value);
}

TEST(V2PageCodecTest, InitializeBuildsValidPageAndClearsPayload) {
    PageV2 page;
    page.data.fill(static_cast<char>(0xFF));
    page.page_num = 0;
    page.refs_num = 3;
    page.is_dirty = true;
    page.need_flushing = true;

    V2PageCodec::initialize(
        page.data,
        0,
        V2PageKind::DatabaseMetadata);

    EXPECT_EQ(byte_value(page.data[0]), 0x53u);
    EXPECT_EQ(byte_value(page.data[1]), 0x4Cu);
    EXPECT_EQ(byte_value(page.data[2]), 0x50u);
    EXPECT_EQ(byte_value(page.data[3]), 0x47u);
    EXPECT_EQ(V2PageCodec::page_num(page.data), 0u);
    EXPECT_EQ(page.page_num, V2PageCodec::page_num(page.data));
    EXPECT_EQ(V2PageCodec::page_lsn(page.data), 0u);
    EXPECT_EQ(
        V2PageCodec::page_kind(page.data),
        V2PageKind::DatabaseMetadata);
    EXPECT_EQ(V2PageCodec::validate(page.data), V2PageCodecResult::Success);

    for (std::size_t offset = V2_PAGE_HEADER_SIZE;
         offset < V2_PAGE_SIZE;
         ++offset) {
        EXPECT_EQ(page.data[offset], '\0');
    }

    EXPECT_EQ(page.refs_num, 3u);
    EXPECT_TRUE(page.is_dirty);
    EXPECT_TRUE(page.need_flushing);
}

TEST(V2PageCodecTest, InitializeUsesBigEndian) {
    PageV2 page;
    page.page_num = 0x01020304u;

    V2PageCodec::initialize(
        page.data,
        page.page_num,
        V2PageKind::BTreeLeaf);

    EXPECT_EQ(byte_value(page.data[PAGE_NUM_OFFSET]), 0x01u);
    EXPECT_EQ(byte_value(page.data[PAGE_NUM_OFFSET + 1]), 0x02u);
    EXPECT_EQ(byte_value(page.data[PAGE_NUM_OFFSET + 2]), 0x03u);
    EXPECT_EQ(byte_value(page.data[PAGE_NUM_OFFSET + 3]), 0x04u);
    EXPECT_EQ(V2PageCodec::page_num(page.data), 0x01020304u);
    EXPECT_EQ(page.page_num, V2PageCodec::page_num(page.data));

    EXPECT_EQ(byte_value(page.data[PAGE_KIND_OFFSET]), 0x00u);
    EXPECT_EQ(byte_value(page.data[PAGE_KIND_OFFSET + 1]), 0x00u);
    EXPECT_EQ(byte_value(page.data[PAGE_KIND_OFFSET + 2]), 0x00u);
    EXPECT_EQ(byte_value(page.data[PAGE_KIND_OFFSET + 3]), 0x04u);

    EXPECT_EQ(V2PageCodec::validate(page.data), V2PageCodecResult::Success);
}

TEST(V2PageCodecTest, PageLsnRequiresChecksumRecomputation) {
    PageV2 page;
    V2PageCodec::initialize(page.data, 7, V2PageKind::Freelist);
    const auto checksum_before = std::array{
        page.data[PAGE_CHECKSUM_OFFSET],
        page.data[PAGE_CHECKSUM_OFFSET + 1],
        page.data[PAGE_CHECKSUM_OFFSET + 2],
        page.data[PAGE_CHECKSUM_OFFSET + 3],
    };

    V2PageCodec::set_page_lsn(page.data, 0x0102030405060708uLL);

    for (std::size_t index = 0; index < 8; ++index) {
        EXPECT_EQ(
            byte_value(page.data[PAGE_LSN_OFFSET + index]),
            static_cast<unsigned int>(index + 1));
    }
    EXPECT_EQ(
        V2PageCodec::page_lsn(page.data),
        0x0102030405060708uLL);
    EXPECT_EQ(page.data[PAGE_CHECKSUM_OFFSET], checksum_before[0]);
    EXPECT_EQ(page.data[PAGE_CHECKSUM_OFFSET + 1], checksum_before[1]);
    EXPECT_EQ(page.data[PAGE_CHECKSUM_OFFSET + 2], checksum_before[2]);
    EXPECT_EQ(page.data[PAGE_CHECKSUM_OFFSET + 3], checksum_before[3]);
    EXPECT_EQ(V2PageCodec::validate(page.data), V2PageCodecResult::ChecksumMismatch);
    V2PageCodec::update_checksum(page.data);
    EXPECT_EQ(V2PageCodec::validate(page.data), V2PageCodecResult::Success);
}

TEST(V2PageCodecTest, PageKindRequiresChecksumRecomputation) {
    PageV2 page;
    V2PageCodec::initialize(page.data, 9, V2PageKind::BTreeLeaf);
    const auto checksum_before = std::array{
        page.data[PAGE_CHECKSUM_OFFSET],
        page.data[PAGE_CHECKSUM_OFFSET + 1],
        page.data[PAGE_CHECKSUM_OFFSET + 2],
        page.data[PAGE_CHECKSUM_OFFSET + 3],
    };

    V2PageCodec::set_page_kind(page.data, V2PageKind::BTreeInternal);

    EXPECT_EQ(
        V2PageCodec::page_kind(page.data),
        V2PageKind::BTreeInternal);
    EXPECT_EQ(page.data[PAGE_CHECKSUM_OFFSET], checksum_before[0]);
    EXPECT_EQ(page.data[PAGE_CHECKSUM_OFFSET + 1], checksum_before[1]);
    EXPECT_EQ(page.data[PAGE_CHECKSUM_OFFSET + 2], checksum_before[2]);
    EXPECT_EQ(page.data[PAGE_CHECKSUM_OFFSET + 3], checksum_before[3]);
    EXPECT_EQ(V2PageCodec::validate(page.data), V2PageCodecResult::ChecksumMismatch);
    V2PageCodec::update_checksum(page.data);
    EXPECT_EQ(V2PageCodec::validate(page.data), V2PageCodecResult::Success);
}

TEST(V2PageCodecTest, ValidateRejectsIncorrectSize) {
    std::array<char, V2_PAGE_SIZE - 1> short_page{};
    std::array<char, V2_PAGE_SIZE + 1> long_page{};

    EXPECT_EQ(
        V2PageCodec::validate(short_page),
        V2PageCodecResult::InvalidSize);
    EXPECT_EQ(
        V2PageCodec::validate(long_page),
        V2PageCodecResult::InvalidSize);
}

TEST(V2PageCodecTest, ValidateRejectsBadMagic) {
    PageV2 page;
    V2PageCodec::initialize(page.data, 1, V2PageKind::BTreeLeaf);
    page.data[0] ^= 0x01;

    EXPECT_EQ(
        V2PageCodec::validate(page.data),
        V2PageCodecResult::InvalidMagic);
}

TEST(V2PageCodecTest, ValidateRejectsZeroAndUnknownPageKinds) {
    PageV2 page;
    V2PageCodec::initialize(page.data, 1, V2PageKind::BTreeLeaf);
    page.data[PAGE_KIND_OFFSET + 3] = 0;

    EXPECT_EQ(
        V2PageCodec::validate(page.data),
        V2PageCodecResult::InvalidPageKind);

    page.data[PAGE_KIND_OFFSET + 3] = 5;
    EXPECT_EQ(
        V2PageCodec::validate(page.data),
        V2PageCodecResult::InvalidPageKind);
}

TEST(V2PageCodecTest, ValidateRejectsPayloadCorruption) {
    PageV2 page;
    V2PageCodec::initialize(page.data, 1, V2PageKind::BTreeLeaf);
    page.data[V2_PAGE_HEADER_SIZE] = static_cast<char>(0xA5);

    EXPECT_EQ(
        V2PageCodec::validate(page.data),
        V2PageCodecResult::ChecksumMismatch);
}

TEST(V2PageCodecTest, ValidateProtectsPageNumber) {
    PageV2 page;
    V2PageCodec::initialize(page.data, 1, V2PageKind::BTreeLeaf);
    page.data[PAGE_NUM_OFFSET + 3] ^= 1;
    EXPECT_EQ(V2PageCodec::validate(page.data), V2PageCodecResult::ChecksumMismatch);
}

TEST(V2PageCodecTest, ValidateStructureIgnoresStaleChecksum) {
    PageV2 page;
    V2PageCodec::initialize(page.data, 1, V2PageKind::BTreeLeaf);
    V2PageCodec::set_page_lsn(page.data, 42);
    EXPECT_EQ(V2PageCodec::validate_structure(page.data), V2PageCodecResult::Success);
}

TEST(V2PageCodecTest, ValidateRejectsStoredChecksumCorruption) {
    PageV2 page;
    V2PageCodec::initialize(page.data, 1, V2PageKind::BTreeLeaf);
    page.data[PAGE_CHECKSUM_OFFSET] ^= 0x01;

    EXPECT_EQ(
        V2PageCodec::validate(page.data),
        V2PageCodecResult::ChecksumMismatch);
}

TEST(V2PageCodecTest, UpdatingChecksumIsIndependentOfStoredChecksumBytes) {
    PageV2 page;
    V2PageCodec::initialize(page.data, 1, V2PageKind::BTreeLeaf);
    const auto original_checksum = std::array{
        page.data[PAGE_CHECKSUM_OFFSET],
        page.data[PAGE_CHECKSUM_OFFSET + 1],
        page.data[PAGE_CHECKSUM_OFFSET + 2],
        page.data[PAGE_CHECKSUM_OFFSET + 3],
    };

    page.data[PAGE_CHECKSUM_OFFSET] = static_cast<char>(0xFF);
    page.data[PAGE_CHECKSUM_OFFSET + 1] = static_cast<char>(0xFF);
    page.data[PAGE_CHECKSUM_OFFSET + 2] = static_cast<char>(0xFF);
    page.data[PAGE_CHECKSUM_OFFSET + 3] = static_cast<char>(0xFF);
    V2PageCodec::update_checksum(page.data);

    EXPECT_EQ(page.data[PAGE_CHECKSUM_OFFSET], original_checksum[0]);
    EXPECT_EQ(page.data[PAGE_CHECKSUM_OFFSET + 1], original_checksum[1]);
    EXPECT_EQ(page.data[PAGE_CHECKSUM_OFFSET + 2], original_checksum[2]);
    EXPECT_EQ(page.data[PAGE_CHECKSUM_OFFSET + 3], original_checksum[3]);
    EXPECT_EQ(V2PageCodec::validate(page.data), V2PageCodecResult::Success);
}

} // namespace

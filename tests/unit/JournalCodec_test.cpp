#include <gtest/gtest.h>

#include <JournalCodec.h>

#include <array>
#include <cstdint>

namespace {

TEST(JournalCodecTest, SerializeAndDeserializeHeaderRoundTrip) {
    JournalHeader header{};
    header.page_count = 17;
    header.nonce = 0x12345678u;
    header.init_db_page_count = 9;

    std::array<char, JOURNAL_HEADER_SIZE> buffer{};
    Journal::serialize_jHeader(header, buffer.data());

    JournalHeader decoded{};
    Journal::deserialize_jHeader(decoded, buffer.data());

    for (int i = 0; i < 8; i++) {
        EXPECT_EQ(decoded.magic[i], header.magic[i]);
    }
    EXPECT_EQ(decoded.page_count, 17u);
    EXPECT_EQ(decoded.nonce, 0x12345678u);
    EXPECT_EQ(decoded.init_db_page_count, 9u);
}

TEST(JournalCodecTest, HeaderSerializationUsesBigEndianForIntegers) {
    JournalHeader header{};
    header.page_count = 0x01020304u;
    header.nonce = 0x11223344u;
    header.init_db_page_count = 0xA1B2C3D4u;

    std::array<char, JOURNAL_HEADER_SIZE> buffer{};
    Journal::serialize_jHeader(header, buffer.data());

    EXPECT_EQ(static_cast<unsigned char>(buffer[8]), 0x01);
    EXPECT_EQ(static_cast<unsigned char>(buffer[9]), 0x02);
    EXPECT_EQ(static_cast<unsigned char>(buffer[10]), 0x03);
    EXPECT_EQ(static_cast<unsigned char>(buffer[11]), 0x04);

    EXPECT_EQ(static_cast<unsigned char>(buffer[12]), 0x11);
    EXPECT_EQ(static_cast<unsigned char>(buffer[13]), 0x22);
    EXPECT_EQ(static_cast<unsigned char>(buffer[14]), 0x33);
    EXPECT_EQ(static_cast<unsigned char>(buffer[15]), 0x44);

    EXPECT_EQ(static_cast<unsigned char>(buffer[16]), 0xA1);
    EXPECT_EQ(static_cast<unsigned char>(buffer[17]), 0xB2);
    EXPECT_EQ(static_cast<unsigned char>(buffer[18]), 0xC3);
    EXPECT_EQ(static_cast<unsigned char>(buffer[19]), 0xD4);
}

TEST(JournalCodecTest, SerializeAndDeserializePageRecordRoundTrip) {
    JournalPageRecord record{};
    record.page_num = 27;
    record.checksum = 0xCAFEBABEu;
    for (int i = 0; i < PAGE_SIZE; i++) {
        record.data[i] = static_cast<char>(i % 251);
    }

    std::array<char, JOURNAL_PAGE_RECORD> buffer{};
    Journal::serialize_jPage_record(record, buffer.data());

    JournalPageRecord decoded{};
    Journal::deserialize_jPage_record(decoded, buffer.data());

    EXPECT_EQ(decoded.page_num, 27u);
    EXPECT_EQ(decoded.checksum, 0xCAFEBABEu);
    for (int i = 0; i < PAGE_SIZE; i++) {
        EXPECT_EQ(decoded.data[i], record.data[i]);
    }
}

TEST(JournalCodecTest, PageRecordSerializationUsesBigEndianForMetadata) {
    JournalPageRecord record{};
    record.page_num = 0x01020304u;
    record.checksum = 0xA1B2C3D4u;

    std::array<char, JOURNAL_PAGE_RECORD> buffer{};
    Journal::serialize_jPage_record(record, buffer.data());

    EXPECT_EQ(static_cast<unsigned char>(buffer[0]), 0x01);
    EXPECT_EQ(static_cast<unsigned char>(buffer[1]), 0x02);
    EXPECT_EQ(static_cast<unsigned char>(buffer[2]), 0x03);
    EXPECT_EQ(static_cast<unsigned char>(buffer[3]), 0x04);

    EXPECT_EQ(static_cast<unsigned char>(buffer[4 + PAGE_SIZE]), 0xA1);
    EXPECT_EQ(static_cast<unsigned char>(buffer[5 + PAGE_SIZE]), 0xB2);
    EXPECT_EQ(static_cast<unsigned char>(buffer[6 + PAGE_SIZE]), 0xC3);
    EXPECT_EQ(static_cast<unsigned char>(buffer[7 + PAGE_SIZE]), 0xD4);
}

TEST(JournalCodecTest, ChecksumUsesNonceAnd200ByteStride) {
    std::array<char, PAGE_SIZE> page{};
    page[3896] = static_cast<char>(5);
    page[3696] = static_cast<char>(7);
    page[100] = static_cast<char>(11);

    std::uint32_t value = Journal::checksum(10u, page);

    EXPECT_EQ(value, 22u);
}

TEST(JournalCodecTest, ValidateJournalRecordChecksumReturnsTrueForMatchingChecksum) {
    JournalHeader header{};
    header.nonce = 91u;

    JournalPageRecord record{};
    record.page_num = 4;
    record.data[3896] = static_cast<char>(3);
    record.data[3696] = static_cast<char>(9);
    record.checksum = Journal::checksum(header.nonce, record.data);

    EXPECT_EQ(Journal::validate_journal_record_checksum(record, header), true);
}

TEST(JournalCodecTest, ValidateJournalRecordChecksumReturnsFalseForMismatchedChecksum) {
    JournalHeader header{};
    header.nonce = 91u;

    JournalPageRecord record{};
    record.page_num = 4;
    record.data[3896] = static_cast<char>(3);
    record.checksum = Journal::checksum(header.nonce, record.data);
    record.data[3896] = static_cast<char>(4);

    EXPECT_EQ(Journal::validate_journal_record_checksum(record, header), false);
}

TEST(JournalCodecTest, ValidateJournalHeaderReturnsTrueForExpectedMagic) {
    JournalHeader header{};

    EXPECT_EQ(Journal::validate_journal_header(header), true);
}

TEST(JournalCodecTest, ValidateJournalHeaderReturnsFalseForCorruptedMagic) {
    JournalHeader header{};
    header.magic[3] = 0x00;

    EXPECT_EQ(Journal::validate_journal_header(header), false);
}

}

#include <gtest/gtest.h>

#include <Log/WalRecordCodec.h>

#include <cstdint>
#include <vector>

TEST(WalRecordCodecTest, EncodeUsesBigEndianAbsoluteLsnFollowedByData) {
    const WalRecord record{
        .lsn = 0x0102030405060708ULL,
        .data = {'a', 'b'},
    };

    const std::vector<char> encoded = WalRecordCodec::encode(record);

    const std::vector<char> expected{
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 'a', 'b',
    };
    EXPECT_EQ(encoded, expected);
}

TEST(WalRecordCodecTest, EmptyDataRoundTrips) {
    const WalRecord record{.lsn = 1, .data = {}};

    const WalRecord decoded = WalRecordCodec::decode(WalRecordCodec::encode(record));

    EXPECT_EQ(decoded.lsn, 1u);
    EXPECT_TRUE(decoded.data.empty());
}

TEST(WalRecordCodecTest, BinaryDataRoundTrips) {
    const WalRecord record{
        .lsn = 42,
        .data = {
            static_cast<char>(0x00),
            static_cast<char>(0xFF),
            static_cast<char>(0x7F),
        },
    };

    const WalRecord decoded = WalRecordCodec::decode(WalRecordCodec::encode(record));

    EXPECT_EQ(decoded.lsn, record.lsn);
    EXPECT_EQ(decoded.data, record.data);
}

TEST(WalRecordCodecTest, ReservedZeroLsnIsRejected) {
    EXPECT_THROW(
        WalRecordCodec::encode(WalRecord{.lsn = 0, .data = {'x'}}),
        std::invalid_argument);

    const std::vector<char> encoded_zero(WalRecordCodec::LSN_SIZE, 0);
    EXPECT_THROW(WalRecordCodec::decode(encoded_zero), std::runtime_error);
}

TEST(WalRecordCodecTest, DecodeRejectsMissingLsnBytes) {
    const std::vector<char> short_record(WalRecordCodec::LSN_SIZE - 1, 0);

    EXPECT_THROW(WalRecordCodec::decode(short_record), std::runtime_error);
}

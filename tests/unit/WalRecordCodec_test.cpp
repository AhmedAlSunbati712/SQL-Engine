#include <gtest/gtest.h>

#include <Log/WalRecordCodec.h>
#include <Endian.h>

#include <vector>

namespace {

WalRecord system_record() {
    return {.lsn = 0x0102030405060708ULL,
            .type = WalRecordType::SystemAction,
            .data = {'a', 'b'}};
}

TEST(WalRecordCodecTest, EncodesFixedBigEndianHeader) {
    const auto encoded = WalRecordCodec::encode(system_record());
    EXPECT_EQ(std::vector<char>(encoded.begin(), encoded.begin() + 4),
              (std::vector<char>{'S', 'L', 'W', 'L'}));
    EXPECT_EQ(get_u16_be(encoded.data() + 4), 1u);
    EXPECT_EQ(get_u16_be(encoded.data() + 6), 4u);
    EXPECT_EQ(get_u32_be(encoded.data() + 8), 42u);
    EXPECT_EQ(get_u64_be(encoded.data() + 16), 0x0102030405060708ULL);
    EXPECT_EQ(get_u64_be(encoded.data() + 24), 0u);
    EXPECT_EQ(get_u64_be(encoded.data() + 32), 0u);
}

TEST(WalRecordCodecTest, RoundTripsEveryEnvelopeField) {
    const WalRecord input{.lsn = 9, .type = WalRecordType::BTreeAction,
                          .transaction_id = 7, .prev_lsn = 8, .data = {0, -1, 2}};
    const auto output = WalRecordCodec::decode(WalRecordCodec::encode(input));
    EXPECT_EQ(output.lsn, input.lsn);
    EXPECT_EQ(output.type, input.type);
    EXPECT_EQ(output.transaction_id, input.transaction_id);
    EXPECT_EQ(output.prev_lsn, input.prev_lsn);
    EXPECT_EQ(output.data, input.data);
}

TEST(WalRecordCodecTest, RejectsCorruptFraming) {
    auto encoded = WalRecordCodec::encode(system_record());
    for (std::size_t offset : {0u, 4u, 6u, 8u, 12u, 20u, 40u}) {
        auto corrupt = encoded;
        corrupt[offset] ^= 1;
        EXPECT_THROW(WalRecordCodec::decode(corrupt), std::runtime_error);
    }
    EXPECT_THROW(WalRecordCodec::decode(std::span(encoded).first(39)), std::runtime_error);
    std::vector<char> old_format(8, 0);
    old_format[7] = 1;
    EXPECT_THROW(WalRecordCodec::decode(old_format), std::runtime_error);
}

TEST(WalRecordCodecTest, EnforcesTransactionFields) {
    EXPECT_THROW(WalRecordCodec::encode({.lsn = 1, .type = WalRecordType::TxnBegin}),
                 std::invalid_argument);
    EXPECT_THROW(WalRecordCodec::encode({.lsn = 1, .type = WalRecordType::TxnBegin,
                                         .transaction_id = 2, .prev_lsn = 3}),
                 std::invalid_argument);
    EXPECT_THROW(WalRecordCodec::encode({.lsn = 1, .type = WalRecordType::TxnCommit,
                                         .transaction_id = 2}),
                 std::invalid_argument);
    EXPECT_THROW(WalRecordCodec::encode({.lsn = 1, .type = WalRecordType::SystemAction,
                                         .transaction_id = 2}),
                 std::invalid_argument);
    EXPECT_THROW(WalRecordCodec::encode({.lsn = 0, .type = WalRecordType::SystemAction}),
                 std::invalid_argument);
}

} // namespace

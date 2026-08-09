#include <gtest/gtest.h>

#include <KeyCodec.h>
#include <NetCodec.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

TEST(NetCodecTest, SerializeKeyUsesStableTypeAndNetworkPayloadSize) {
    const Key key = KeyCodec::make_uint64(0x0102030405060708ULL);

    const std::vector<std::uint8_t> encoded = NetCodec::serialize_key(key);

    const std::vector<std::uint8_t> expected = {
        2, 0, 0, 0,
        0, 0, 0, 8,
        1, 2, 3, 4, 5, 6, 7, 8
    };
    EXPECT_EQ(encoded, expected);
}

TEST(NetCodecTest, KeyTypesRoundTrip) {
    const std::vector<Key> keys = {
        KeyCodec::make_bool(true),
        KeyCodec::make_uint64(42),
        KeyCodec::make_int64(-42),
        KeyCodec::make_string(std::string{"key\0text", 8}),
        KeyCodec::make_bytes(std::vector<char>{'b', '\0', 'y'})
    };

    for (const Key& key : keys) {
        const Key decoded = NetCodec::deserialize_key(
            NetCodec::serialize_key(key)
        );
        EXPECT_TRUE(KeyCodec::equal(decoded, key));
        EXPECT_EQ(decoded.size, key.size);
    }
}

TEST(NetCodecTest, SerializeKeyRejectsInvalidInput) {
    Key invalid = KeyCodec::make_string("key");
    invalid.size = 4;

    EXPECT_THROW(NetCodec::serialize_key(invalid), std::invalid_argument);
}

TEST(NetCodecTest, DeserializeKeyRejectsMalformedBuffers) {
    EXPECT_THROW(
        NetCodec::deserialize_key(std::vector<std::uint8_t>(7, 0)),
        std::runtime_error
    );

    std::vector<std::uint8_t> unknown_type = {
        99, 0, 0, 0,
        0, 0, 0, 0
    };
    EXPECT_THROW(NetCodec::deserialize_key(unknown_type), std::runtime_error);

    std::vector<std::uint8_t> nonzero_reserved = {
        4, 1, 0, 0,
        0, 0, 0, 0
    };
    EXPECT_THROW(
        NetCodec::deserialize_key(nonzero_reserved),
        std::runtime_error
    );

    std::vector<std::uint8_t> wrong_size = {
        4, 0, 0, 0,
        0, 0, 0, 2,
        'x'
    };
    EXPECT_THROW(NetCodec::deserialize_key(wrong_size), std::runtime_error);

    std::vector<std::uint8_t> invalid_bool = {
        1, 0, 0, 0,
        0, 0, 0, 1,
        'x'
    };
    EXPECT_THROW(NetCodec::deserialize_key(invalid_bool), std::runtime_error);
}

} // namespace

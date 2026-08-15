#include <Log/WalRecordCodec.h>

#include <Crc32c.h>
#include <Endian.h>

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace {

constexpr std::array<char, 4> WAL_MAGIC{'S', 'L', 'W', 'L'};
constexpr std::size_t CHECKSUM_OFFSET = 12;

bool is_known_type(WalRecordType type) {
    return type >= WalRecordType::TxnBegin && type <= WalRecordType::TxnEnd;
}

void validate_fields(const WalRecord& record, bool decoding) {
    auto fail = [decoding](const char* message) {
        if (decoding) throw std::runtime_error(message);
        throw std::invalid_argument(message);
    };

    if (record.lsn == 0) fail("WAL record LSN zero is reserved for none");
    if (!is_known_type(record.type)) fail("WAL record type is unknown");

    if (record.type == WalRecordType::SystemAction) {
        if (record.transaction_id != 0 || record.prev_lsn != 0) {
            fail("System action must not belong to a transaction");
        }
        return;
    }

    if (record.transaction_id == 0) fail("Transactional WAL record requires a transaction ID");
    if (record.type == WalRecordType::TxnBegin) {
        if (record.prev_lsn != 0) fail("Transaction begin must have prevLSN zero");
    } else if (record.prev_lsn == 0) {
        fail("Transactional WAL record requires a nonzero prevLSN");
    }
}

std::uint32_t record_checksum(std::span<const char> encoded) {
    std::vector<char> copy(encoded.begin(), encoded.end());
    std::fill_n(copy.begin() + CHECKSUM_OFFSET, sizeof(std::uint32_t), '\0');
    return crc32c(copy);
}

} // namespace

namespace WalRecordCodec {

std::vector<char> encode(const WalRecord& record) {
    validate_fields(record, false);
    if (record.data.size() > std::numeric_limits<std::uint32_t>::max() - HEADER_SIZE) {
        throw std::invalid_argument("WAL record is too large");
    }

    std::vector<char> encoded(HEADER_SIZE + record.data.size(), 0);
    std::copy(WAL_MAGIC.begin(), WAL_MAGIC.end(), encoded.begin());
    put_u16_be(encoded.data() + 4, FORMAT_VERSION);
    put_u16_be(encoded.data() + 6, static_cast<std::uint16_t>(record.type));
    put_u32_be(encoded.data() + 8, static_cast<std::uint32_t>(encoded.size()));
    put_u64_be(encoded.data() + 16, record.lsn);
    put_u64_be(encoded.data() + 24, record.transaction_id);
    put_u64_be(encoded.data() + 32, record.prev_lsn);
    std::copy(record.data.begin(), record.data.end(), encoded.begin() + HEADER_SIZE);
    put_u32_be(encoded.data() + CHECKSUM_OFFSET, record_checksum(encoded));
    return encoded;
}

WalRecord decode(std::span<const char> encoded) {
    if (encoded.size() < HEADER_SIZE) throw std::runtime_error("Encoded WAL record is truncated");
    if (!std::equal(WAL_MAGIC.begin(), WAL_MAGIC.end(), encoded.begin())) {
        throw std::runtime_error("Encoded WAL record has invalid magic");
    }
    if (get_u16_be(encoded.data() + 4) != FORMAT_VERSION) {
        throw std::runtime_error("Encoded WAL record has unsupported version");
    }
    if (get_u32_be(encoded.data() + 8) != encoded.size()) {
        throw std::runtime_error("Encoded WAL record size disagrees with framing");
    }
    if (get_u32_be(encoded.data() + CHECKSUM_OFFSET) != record_checksum(encoded)) {
        throw std::runtime_error("Encoded WAL record checksum mismatch");
    }

    WalRecord record{
        .lsn = get_u64_be(encoded.data() + 16),
        .type = static_cast<WalRecordType>(get_u16_be(encoded.data() + 6)),
        .transaction_id = get_u64_be(encoded.data() + 24),
        .prev_lsn = get_u64_be(encoded.data() + 32),
        .data = std::vector<char>(encoded.begin() + HEADER_SIZE, encoded.end()),
    };
    validate_fields(record, true);
    return record;
}

} // namespace WalRecordCodec

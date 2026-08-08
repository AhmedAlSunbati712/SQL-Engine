#include <Log/WalRecordCodec.h>

#include <Endian.h>

#include <algorithm>
#include <stdexcept>

namespace WalRecordCodec {

std::vector<char> encode(const WalRecord &record) {
    if (record.lsn == 0) {
        throw std::invalid_argument("WAL record LSN zero is reserved for none");
    }

    std::vector<char> encoded(LSN_SIZE + record.data.size());
    put_u64_be(encoded.data(), record.lsn);
    std::copy(record.data.begin(), record.data.end(), encoded.begin() + LSN_SIZE);
    return encoded;
}

WalRecord decode(std::span<const char> encoded) {
    if (encoded.size() < LSN_SIZE) {
        throw std::runtime_error("Encoded WAL record is shorter than its LSN prefix");
    }

    WalRecord record;
    record.lsn = get_u64_be(encoded.data());
    if (record.lsn == 0) {
        throw std::runtime_error("Encoded WAL record uses reserved LSN zero");
    }

    record.data.assign(encoded.begin() + LSN_SIZE, encoded.end());
    return record;
}

} // namespace WalRecordCodec

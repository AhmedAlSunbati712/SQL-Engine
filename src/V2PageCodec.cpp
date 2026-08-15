#include <V2PageCodec.h>

#include <Endian.h>
#include <Crc32c.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace {


bool is_valid_page_kind(V2PageKind kind) {
    switch (kind) {
        case V2PageKind::DatabaseMetadata:
        case V2PageKind::Freelist:
        case V2PageKind::BTreeInternal:
        case V2PageKind::BTreeLeaf:
            return true;
    }

    return false;
}

std::uint32_t compute_checksum(std::span<const char, V2_PAGE_SIZE> page) {
    std::array<char, V2_PAGE_SIZE> copy{};
    std::copy(page.begin(), page.end(), copy.begin());
    std::fill_n(copy.begin() + PAGE_CHECKSUM_OFFSET, PAGE_CHECKSUM_SIZE, '\0');
    return crc32c(copy);
}

} // namespace

namespace V2PageCodec {

void initialize(std::span<char, V2_PAGE_SIZE> page, std::uint32_t page_num_value, V2PageKind kind) {
    assert(is_valid_page_kind(kind));

    std::fill(page.begin(), page.end(), '\0');
    std::copy(PAGE_MAGIC.begin(), PAGE_MAGIC.end(), page.begin() + PAGE_MAGIC_OFFSET);
    put_u32_be(page.data() + PAGE_NUM_OFFSET, page_num_value);
    put_u64_be(page.data() + PAGE_LSN_OFFSET, 0);
    put_u32_be(page.data() + PAGE_KIND_OFFSET, static_cast<std::uint32_t>(kind));
    update_checksum(page);
}

std::uint32_t page_num(std::span<const char, V2_PAGE_SIZE> page) {
    return get_u32_be(page.data() + PAGE_NUM_OFFSET);
}

std::uint64_t page_lsn(std::span<const char, V2_PAGE_SIZE> page) {
    return get_u64_be(page.data() + PAGE_LSN_OFFSET);
}

V2PageKind page_kind(std::span<const char, V2_PAGE_SIZE> page) {
    return static_cast<V2PageKind>(get_u32_be(page.data() + PAGE_KIND_OFFSET));
}

void set_page_lsn(std::span<char, V2_PAGE_SIZE> page, std::uint64_t lsn) {
    put_u64_be(page.data() + PAGE_LSN_OFFSET, lsn);
}

void set_page_kind(std::span<char, V2_PAGE_SIZE> page, V2PageKind kind) {
    assert(is_valid_page_kind(kind));

    put_u32_be(
        page.data() + PAGE_KIND_OFFSET,
        static_cast<std::uint32_t>(kind));
}

void update_checksum(std::span<char, V2_PAGE_SIZE> page) {
    const std::span<const char, V2_PAGE_SIZE> const_page{page};
    put_u32_be(page.data() + PAGE_CHECKSUM_OFFSET, compute_checksum(const_page));
}

V2PageCodecResult validate_structure(std::span<const char> page) {
    if (page.size() != V2_PAGE_SIZE) {
        return V2PageCodecResult::InvalidSize;
    }

    const std::span<const char, V2_PAGE_SIZE> full_page{
        page.data(),
        V2_PAGE_SIZE,
    };

    if (!std::equal(PAGE_MAGIC.begin(), PAGE_MAGIC.end(), full_page.begin() + PAGE_MAGIC_OFFSET)) return V2PageCodecResult::InvalidMagic;

    if (!is_valid_page_kind(page_kind(full_page))) return V2PageCodecResult::InvalidPageKind;

    return V2PageCodecResult::Success;
}

V2PageCodecResult validate(std::span<const char> page) {
    const auto structure = validate_structure(page);
    if (structure != V2PageCodecResult::Success) return structure;

    const std::span<const char, V2_PAGE_SIZE> full_page{page.data(), V2_PAGE_SIZE};
    const auto stored = get_u32_be(full_page.data() + PAGE_CHECKSUM_OFFSET);
    if (stored != compute_checksum(full_page)) return V2PageCodecResult::ChecksumMismatch;
    return V2PageCodecResult::Success;
}

} // namespace V2PageCodec

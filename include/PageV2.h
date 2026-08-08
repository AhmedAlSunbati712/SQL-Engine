#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

/// Exact byte size of every persistent V2 database page.
inline constexpr std::size_t V2_PAGE_SIZE = 4096;

/// Byte size of the common persistent header at the start of every V2 page.
inline constexpr std::size_t V2_PAGE_HEADER_SIZE = 24;

/// Byte size reserved for the page-kind-specific, opaque payload.
inline constexpr std::size_t V2_PAGE_PAYLOAD_SIZE =
    V2_PAGE_SIZE - V2_PAGE_HEADER_SIZE;

/// Persistent page-kind values stored in bytes 20..23 of a V2 page.
///
/// These numeric values are part of the on-disk format. Zero and values not
/// listed here are invalid. Do not renumber existing values without making an
/// intentional, incompatible page-format change.
enum class V2PageKind : std::uint32_t {
    DatabaseMetadata = 1,
    Freelist = 2,
    BTreeInternal = 3,
    BTreeLeaf = 4,
};

/// Pager-owned in-memory container for one V2 database page.
///
/// `data` is the sole authoritative persistent page image. Page kind, pageLSN,
/// checksum, and payload are encoded directly in these bytes and are not
/// duplicated as independently mutable C++ fields.
///
/// `page_num` mirrors the encoded page number for legacy-compatible cache
/// lookup. The pager must populate it after initialization or validation and
/// ensure it equals V2PageCodec::page_num(data). It and the remaining cache
/// members are runtime-only state and are never serialized. A
/// default-constructed instance contains all-zero bytes, which is not a valid
/// persistent page until V2PageCodec::initialize is called.
struct PageV2 {
    std::array<char, V2_PAGE_SIZE> data{};

    std::uint32_t page_num = 0;
    std::uint32_t refs_num = 0;
    bool is_dirty = false;
    bool need_flushing = false;
};

static_assert(V2_PAGE_HEADER_SIZE == 24);
static_assert(V2_PAGE_PAYLOAD_SIZE == 4072);
static_assert(sizeof(PageV2::data) == V2_PAGE_SIZE);
static_assert(static_cast<std::uint32_t>(V2PageKind::DatabaseMetadata) == 1);
static_assert(static_cast<std::uint32_t>(V2PageKind::Freelist) == 2);
static_assert(static_cast<std::uint32_t>(V2PageKind::BTreeInternal) == 3);
static_assert(static_cast<std::uint32_t>(V2PageKind::BTreeLeaf) == 4);

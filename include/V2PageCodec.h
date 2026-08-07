#pragma once

#include <PageV2.h>

#include <cstddef>
#include <cstdint>
#include <span>


constexpr std::size_t PAGE_MAGIC_OFFSET = 0;
constexpr std::size_t PAGE_NUM_OFFSET = 4;
constexpr std::size_t PAGE_LSN_OFFSET = 8;
constexpr std::size_t PAGE_CHECKSUM_OFFSET = 16;
constexpr std::size_t PAGE_KIND_OFFSET = 20;
constexpr std::size_t PAGE_CHECKSUM_SIZE = sizeof(std::uint32_t);

constexpr std::array<char, 4> PAGE_MAGIC{
    'S',
    'L',
    'P',
    'G',
};

constexpr std::uint32_t CRC32C_POLYNOMIAL = 0x82F63B78u;
constexpr std::uint32_t CRC32C_INITIAL = 0xFFFFFFFFu;

/// Result of validating a complete serialized V2 page image.
///
/// Callers may use the individual failures for diagnostics or translate all
/// corruption failures into a subsystem-level status. When multiple fields
/// are corrupt, the specific failure returned is intentionally unspecified.
enum class V2PageCodecResult : std::uint8_t {
    Success = 0,
    InvalidSize,
    InvalidMagic,
    InvalidPageKind,
    ChecksumMismatch,
};

/// Encodes, decodes, and validates the common persistent V2 page header.
///
/// The 4096-byte page layout is:
///
///     bytes  0..3  ASCII magic "SLPG"
///     bytes  4..7  page number
///     bytes  8..15 pageLSN
///     bytes 16..19 CRC32C
///     bytes 20..23 page kind
///     bytes 24..4095 page-kind-specific opaque payload
///
/// Persistent integers use big-endian byte order. Page number zero is valid
/// and identifies the database metadata page. A pageLSN of zero means that no
/// WAL update has yet been assigned to the page.
namespace V2PageCodec {

    /// Initializes a complete page image and leaves it checksum-valid.
    ///
    /// All 4096 bytes are cleared before the magic, page number, zero pageLSN,
    /// page kind, and checksum are written. `kind` must name one of the four
    /// declared V2PageKind enumerators.
    void initialize(std::span<char, V2_PAGE_SIZE> page, std::uint32_t page_num, V2PageKind kind);

    /// Reads the page number from an exact-size page image.
    ///
    /// This function decodes bytes but does not validate untrusted input. A
    /// disk-loaded page must pass validate before its decoded fields are used.
    std::uint32_t page_num(std::span<const char, V2_PAGE_SIZE> page);

    /// Reads the pageLSN from an exact-size page image without validating it.
    std::uint64_t page_lsn(std::span<const char, V2_PAGE_SIZE> page);

    /// Reads the page kind from an exact-size page image without validating it.
    V2PageKind page_kind(std::span<const char, V2_PAGE_SIZE> page);

    /// Writes a pageLSN in big-endian order.
    ///
    /// The pageLSN lies outside the checksummed payload, so this write does not
    /// change the stored checksum.
    void set_page_lsn(std::span<char, V2_PAGE_SIZE> page, std::uint64_t lsn);

    /// Writes a valid page kind in big-endian order.
    ///
    /// `kind` must name one of the four declared V2PageKind enumerators. Page
    /// kind lies outside the checksummed payload.
    void set_page_kind(std::span<char, V2_PAGE_SIZE> page, V2PageKind kind);

    /// Recomputes and stores the page's CRC32C checksum.
    ///
    /// CRC32C uses the Castagnoli polynomial and covers only the 4072-byte
    /// page-kind payload at bytes 24..4095.
    void update_checksum(std::span<char, V2_PAGE_SIZE> page);

    /// Validates a complete serialized V2 page image.
    ///
    /// Validation checks exact size, magic, page kind, and payload CRC32C
    /// without modifying `page`.
    V2PageCodecResult validate(std::span<const char> page);

} // namespace V2PageCodec

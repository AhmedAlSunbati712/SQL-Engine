#pragma once

#include <cstddef>
#include <cstdint>
#include <shared_mutex>

/// Result of inspecting the fixed-width entries in a WAL index file.
enum class IndexScanStatus : std::uint8_t {
    Complete = 0,
    IncompleteTail,
    Corrupt,
};

/// Describes the valid prefix of a WAL index file.
///
/// `valid_size` stops before an incomplete final entry or the first entry whose
/// encoded relative LSN does not match its ordinal position. The Store remains
/// authoritative; Segment later validates that indexed offsets identify real
/// Store records and rebuilds an invalid Index from those records.
struct IndexScanResult {
    IndexScanStatus status = IndexScanStatus::Complete;
    std::uint64_t physical_size = 0;
    std::uint64_t valid_size = 0;
    std::uint64_t entry_count = 0;
};

/// Owns the fixed-width index file for one WAL segment.
///
/// Entries are dense and begin at relative LSN zero:
///
/// ```text
/// +--------------------------+--------------------------+
/// | relative LSN (4 bytes)   | Store offset (8 bytes)   |
/// | unsigned, big-endian     | unsigned, big-endian     |
/// +--------------------------+--------------------------+
/// ```
///
/// Because entries have a fixed width, lookup is a positional read at
/// `relative_lsn * ENTRY_SIZE`. The constructor takes ownership of `fd` and
/// inspects existing entries. A partial final entry blocks append until
/// `repair_tail()` truncates it. A corrupt complete entry requires a later
/// rebuild from the authoritative Store and cannot be repaired here.
///
/// Reads may run concurrently. Append, repair, and synchronization are
/// serialized. Append does not make an entry crash-durable; the caller uses
/// `sync()` at the appropriate WAL durability boundary.
class Index {
    public:
        static constexpr std::size_t RELATIVE_LSN_SIZE = sizeof(std::uint32_t);
        static constexpr std::size_t STORE_OFFSET_SIZE = sizeof(std::uint64_t);
        static constexpr std::size_t ENTRY_SIZE = RELATIVE_LSN_SIZE + STORE_OFFSET_SIZE;

        /// Takes ownership of an open, writable index file descriptor.
        explicit Index(int fd);
        ~Index() noexcept;

        Index(const Index &) = delete;
        Index &operator=(const Index &) = delete;
        Index(Index &&) = delete;
        Index &operator=(Index &&) = delete;

        /// Appends the next dense relative-LSN-to-Store-offset mapping.
        ///
        /// `relative_lsn` must equal the current entry count. Throws when the
        /// sequence is not dense, the Index has an incomplete or corrupt tail,
        /// or an I/O operation fails.
        void append(std::uint32_t relative_lsn, std::uint64_t store_offset);

        /// Returns the Store offset mapped by `relative_lsn`.
        ///
        /// Throws when the relative LSN is outside the valid entry prefix, the
        /// stored entry does not match the requested LSN, or an I/O fails.
        std::uint64_t read(std::uint32_t relative_lsn) const;

        /// Returns the current fixed-width entry inspection result.
        IndexScanResult scan() const;

        /// Truncates an incomplete final entry, if present.
        ///
        /// Corrupt complete entries are not repairable by Index. Truncation is
        /// not synchronized automatically; call `sync()` when required.
        void repair_tail();

        /// Synchronizes all current index bytes to the required storage layer.
        void sync();

        /// Returns the current physical index-file size in bytes.
        std::uint64_t size() const;

    private:
        int fd_ = -1;
        std::uint64_t size_ = 0;
        IndexScanResult scan_result_{};
        mutable std::shared_mutex mutex_;

        IndexScanResult inspect_file() const;
};

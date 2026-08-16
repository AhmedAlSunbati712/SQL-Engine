#pragma once

#include <PageLatchManager.h>
#include <TransactionManager/Transaction.h>

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <variant>

// Owns every logical page latch retained by one B+ tree call. Pager references
// remain the caller's responsibility and may be released while a latch stays held.
class BTreeOperation {
public:
    BTreeOperation(TransactionId txn_id, PageLatchManager& latch_manager);
    ~BTreeOperation() noexcept;

    BTreeOperation(const BTreeOperation&) = delete;
    BTreeOperation& operator=(const BTreeOperation&) = delete;
    BTreeOperation(BTreeOperation&&) = delete;
    BTreeOperation& operator=(BTreeOperation&&) = delete;

    void lock_shared(std::uint32_t page_num);
    void lock_exclusive(std::uint32_t page_num);

    void release(std::uint32_t page_num) noexcept;
    void release_all_exclusive_except(std::uint32_t page_num) noexcept;
    void release_all() noexcept;

    std::optional<PageLatchMode> latch_mode(std::uint32_t page_num) const noexcept;
    TransactionId transaction_id() const noexcept { return txn_id_; }

private:
    struct HeldPageLatch {
        PageLatchMode mode = PageLatchMode::Shared;
        std::variant<PageReadLatch, PageWriteLatch> lock;
    };

    TransactionId txn_id_;
    PageLatchManager& latch_manager_;
    std::unordered_map<std::uint32_t, HeldPageLatch> held_latches_;
};

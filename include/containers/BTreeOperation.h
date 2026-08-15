#pragma once

#include <PageV2.h>
#include <TransactionManager/Transaction.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <variant>
#include <vector>

class Pager;

enum class PageLatchMode : std::uint8_t {
    Shared = 0,
    Exclusive,
};

// Owns every page latch and pager reference retained by one B+ tree call.
// Releasing a held page always unlocks its latch before unreferencing it.
class BTreeOperation {
public:
    BTreeOperation(TransactionId txn_id, Pager& pager);
    ~BTreeOperation() noexcept;

    BTreeOperation(const BTreeOperation&) = delete;
    BTreeOperation& operator=(const BTreeOperation&) = delete;
    BTreeOperation(BTreeOperation&&) = delete;
    BTreeOperation& operator=(BTreeOperation&&) = delete;

    // Takes ownership of the caller's existing pager reference.
    PageV2* lock_shared(PageV2* pinned_page);
    PageV2* lock_exclusive(PageV2* pinned_page);

    void release(std::uint32_t page_num) noexcept;
    void release_all() noexcept;

    std::optional<PageLatchMode> latch_mode(std::uint32_t page_num) const noexcept;
    TransactionId transaction_id() const noexcept { return txn_id_; }

private:
    using SharedLatch = std::shared_lock<std::shared_mutex>;
    using ExclusiveLatch = std::unique_lock<std::shared_mutex>;

    struct HeldPage {
        PageV2* page = nullptr;
        PageLatchMode mode = PageLatchMode::Shared;
        std::variant<SharedLatch, ExclusiveLatch> lock;
    };

    TransactionId txn_id_;
    Pager& pager_;
    std::vector<HeldPage> held_pages_;
    std::unordered_map<std::uint32_t, PageLatchMode> held_modes_;
};

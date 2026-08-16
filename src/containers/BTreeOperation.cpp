#include <containers/BTreeOperation.h>

#include <stdexcept>
#include <utility>

BTreeOperation::BTreeOperation(
    TransactionId txn_id,
    PageLatchManager& latch_manager
) : txn_id_(txn_id),
    latch_manager_(latch_manager) {}

BTreeOperation::~BTreeOperation() noexcept {
    release_all();
}

void BTreeOperation::lock_shared(std::uint32_t page_num) {
    auto held = held_latches_.find(page_num);
    if (held != held_latches_.end()) {
        // An exclusive latch already permits reads, while acquiring the same
        // shared latch again would only attempt to recursively lock the page.
        return;
    }

    PageReadLatch latch = latch_manager_.lock_shared(page_num);
    held_latches_.emplace(
        page_num,
        HeldPageLatch{
            .mode = PageLatchMode::Shared,
            .lock = std::move(latch),
        });
}

void BTreeOperation::lock_exclusive(std::uint32_t page_num) {
    auto held = held_latches_.find(page_num);
    if (held != held_latches_.end()) {
        // Exclusive reacquisition is harmless, but std::shared_mutex cannot
        // atomically promote a shared owner without risking self-deadlock.
        if (held->second.mode == PageLatchMode::Exclusive) return;
        throw std::logic_error("Cannot promote a held shared page latch");
    }

    PageWriteLatch latch = latch_manager_.lock_exclusive(page_num);
    held_latches_.emplace(
        page_num,
        HeldPageLatch{
            .mode = PageLatchMode::Exclusive,
            .lock = std::move(latch),
        });
}

void BTreeOperation::release(std::uint32_t page_num) noexcept {
    held_latches_.erase(page_num);
}

void BTreeOperation::release_all() noexcept {
    held_latches_.clear();
}

std::optional<PageLatchMode> BTreeOperation::latch_mode(
    std::uint32_t page_num
) const noexcept {
    auto held = held_latches_.find(page_num);
    if (held == held_latches_.end()) return std::nullopt;
    return held->second.mode;
}

#include <containers/BTreeOperation.h>

#include <Pager.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

BTreeOperation::BTreeOperation(TransactionId txn_id, Pager& pager)
    : txn_id_(txn_id),
      pager_(pager) {}

BTreeOperation::~BTreeOperation() noexcept {
    release_all();
}

PageV2* BTreeOperation::lock_shared(PageV2* pinned_page) {
    if (!pinned_page) {
        throw std::invalid_argument("Cannot latch a null page");
    }

    // Acquiring the same page twice could make this thread wait on a latch it
    // already owns. Reject the request before attempting another acquisition.
    if (held_modes_.contains(pinned_page->page_num)) {
        (void)pager_.unref_page(static_cast<int>(pinned_page->page_num));
        throw std::logic_error("BTree operation already holds this page");
    }

    // The operation owns the caller's pager reference as soon as this method
    // is called, so every failed acquisition must release that reference.
    try {
        SharedLatch lock(pinned_page->latch);
        held_pages_.push_back({
            .page = pinned_page,
            .mode = PageLatchMode::Shared,
            .lock = std::move(lock),
        });
        try {
            held_modes_.emplace(pinned_page->page_num, PageLatchMode::Shared);
        } catch (...) {
            held_pages_.pop_back();
            throw;
        }
    } catch (...) {
        (void)pager_.unref_page(static_cast<int>(pinned_page->page_num));
        throw;
    }

    return pinned_page;
}

PageV2* BTreeOperation::lock_exclusive(PageV2* pinned_page) {
    if (!pinned_page) {
        throw std::invalid_argument("Cannot latch a null page");
    }

    // Page latches are not recursive and are never promoted in place.
    if (held_modes_.contains(pinned_page->page_num)) {
        (void)pager_.unref_page(static_cast<int>(pinned_page->page_num));
        throw std::logic_error("BTree operation already holds this page");
    }

    try {
        ExclusiveLatch lock(pinned_page->latch);
        held_pages_.push_back({
            .page = pinned_page,
            .mode = PageLatchMode::Exclusive,
            .lock = std::move(lock),
        });
        try {
            held_modes_.emplace(pinned_page->page_num, PageLatchMode::Exclusive);
        } catch (...) {
            held_pages_.pop_back();
            throw;
        }
    } catch (...) {
        (void)pager_.unref_page(static_cast<int>(pinned_page->page_num));
        throw;
    }

    return pinned_page;
}

void BTreeOperation::release(std::uint32_t page_num) noexcept {
    auto held = std::find_if(
        held_pages_.begin(),
        held_pages_.end(),
        [&](const HeldPage& page) {
            return page.page->page_num == page_num;
        });
    if (held == held_pages_.end()) return;

    // Unlock before erasing the guard and dropping the pager reference.
    std::visit([](auto& lock) {
        if (lock.owns_lock()) lock.unlock();
    }, held->lock);
    held_pages_.erase(held);
    held_modes_.erase(page_num);
    (void)pager_.unref_page(static_cast<int>(page_num));
}

void BTreeOperation::release_all() noexcept {
    // Release descendants before ancestors by unwinding acquisition order.
    while (!held_pages_.empty()) {
        HeldPage& held = held_pages_.back();
        const std::uint32_t page_num = held.page->page_num;

        std::visit([](auto& lock) {
            if (lock.owns_lock()) lock.unlock();
        }, held.lock);
        held_pages_.pop_back();
        held_modes_.erase(page_num);
        (void)pager_.unref_page(static_cast<int>(page_num));
    }
}

std::optional<PageLatchMode> BTreeOperation::latch_mode(
    std::uint32_t page_num) const noexcept {
    auto mode = held_modes_.find(page_num);
    if (mode == held_modes_.end()) return std::nullopt;
    return mode->second;
}

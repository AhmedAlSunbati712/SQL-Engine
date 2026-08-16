#include <PageLatchManager.h>

#include <functional>
#include <utility>

PageReadLatch::PageReadLatch(
    std::uint32_t page_num,
    std::shared_ptr<PageLatchState> state
) : page_num_(page_num),
    state_(std::move(state)),
    lock_(state_->mutex) {}

PageWriteLatch::PageWriteLatch(
    std::uint32_t page_num,
    std::shared_ptr<PageLatchState> state
) : page_num_(page_num),
    state_(std::move(state)),
    lock_(state_->mutex) {}

PageReadLatch PageLatchManager::lock_shared(std::uint32_t page_num) {
    // Resolve the shared latch state without holding the shard mutex while
    // waiting for another operation to release the actual page latch.
    std::shared_ptr<PageLatchState> state = state_for(page_num);
    return PageReadLatch(page_num, std::move(state));
}

PageWriteLatch PageLatchManager::lock_exclusive(std::uint32_t page_num) {
    // Exclusive page access uses the same logical state as shared access so
    // all operations addressing this page number synchronize with each other.
    std::shared_ptr<PageLatchState> state = state_for(page_num);
    return PageWriteLatch(page_num, std::move(state));
}

std::shared_ptr<PageLatchState> PageLatchManager::state_for(
    std::uint32_t page_num
) {
    const std::size_t hash = std::hash<std::uint32_t>{}(page_num);
    PageLatchShard& shard = shards_[hash % SHARD_COUNT];
    std::lock_guard lock(shard.mutex);

    auto existing = shard.states.find(page_num);
    if (existing != shard.states.end()) {
        std::shared_ptr<PageLatchState> state = existing->second.lock();
        if (state) return state;
        shard.states.erase(existing);
    }

    // Weak entries do not keep unused latch states alive. Periodically remove
    // expired entries so page numbers touched only once do not grow the shard.
    shard.acquisitions_since_cleanup++;
    if (shard.acquisitions_since_cleanup >= CLEANUP_INTERVAL) {
        for (auto state = shard.states.begin(); state != shard.states.end();) {
            if (state->second.expired()) {
                state = shard.states.erase(state);
            } else {
                ++state;
            }
        }
        shard.acquisitions_since_cleanup = 0;
    }

    std::shared_ptr<PageLatchState> state = std::make_shared<PageLatchState>();
    shard.states.emplace(page_num, state);
    return state;
}

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

enum class PageLatchMode : std::uint8_t {
    Shared = 0,
    Exclusive,
};

struct PageLatchState {
    std::shared_mutex mutex;
};

class PageReadLatch {
public:
    PageReadLatch(PageReadLatch&&) noexcept = default;
    PageReadLatch& operator=(PageReadLatch&&) noexcept = default;

    PageReadLatch(const PageReadLatch&) = delete;
    PageReadLatch& operator=(const PageReadLatch&) = delete;

    std::uint32_t page_num() const noexcept { return page_num_; }
    bool owns_lock() const noexcept { return lock_.owns_lock(); }

private:
    friend class PageLatchManager;

    PageReadLatch(
        std::uint32_t page_num,
        std::shared_ptr<PageLatchState> state
    );

    std::uint32_t page_num_;
    std::shared_ptr<PageLatchState> state_;
    std::shared_lock<std::shared_mutex> lock_;
};

class PageWriteLatch {
public:
    PageWriteLatch(PageWriteLatch&&) noexcept = default;
    PageWriteLatch& operator=(PageWriteLatch&&) noexcept = default;

    PageWriteLatch(const PageWriteLatch&) = delete;
    PageWriteLatch& operator=(const PageWriteLatch&) = delete;

    std::uint32_t page_num() const noexcept { return page_num_; }
    bool owns_lock() const noexcept { return lock_.owns_lock(); }

private:
    friend class PageLatchManager;

    PageWriteLatch(
        std::uint32_t page_num,
        std::shared_ptr<PageLatchState> state
    );

    std::uint32_t page_num_;
    std::shared_ptr<PageLatchState> state_;
    std::unique_lock<std::shared_mutex> lock_;
};

struct PageLatchShard {
    std::mutex mutex;
    std::unordered_map<std::uint32_t, std::weak_ptr<PageLatchState>> states;
    std::size_t acquisitions_since_cleanup = 0;
};

class PageLatchManager {
public:
    PageLatchManager() = default;
    ~PageLatchManager() = default;

    PageLatchManager(const PageLatchManager&) = delete;
    PageLatchManager& operator=(const PageLatchManager&) = delete;
    PageLatchManager(PageLatchManager&&) = delete;
    PageLatchManager& operator=(PageLatchManager&&) = delete;

    PageReadLatch lock_shared(std::uint32_t page_num);
    PageWriteLatch lock_exclusive(std::uint32_t page_num);

private:
    static constexpr std::size_t SHARD_COUNT = 64;
    static constexpr std::size_t CLEANUP_INTERVAL = 64;

    std::array<PageLatchShard, SHARD_COUNT> shards_;

    std::shared_ptr<PageLatchState> state_for(std::uint32_t page_num);
};

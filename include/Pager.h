#pragma once
#include <cstdint>
#include <Page.h>
#include <PageV2.h>
#include <PageLatchManager.h>
#include <Log/WalPayload.h>
#include <unordered_set>
#include <vector>
#include <mutex>
#include <PCache.h>

class BTreeOperation;
class Log;

enum class PagerResult : std::uint8_t {
    Success = 0,
    DatabaseNotOpen,
    OpenDbFailed,
    DbReadFailed,
    PageCorrupt,
    InvalidPageKind,
    PageOutOfRange,
    DbHeaderCorrupt,
    NoEvictablePage,
    CacheSpillFailed,
    DbWriteFailed,
    DbFlushFailed,
    Busy,
    PageNotCached,
    EmptyBTree,
    LogNotAttached
};

struct PagerGetResult {
    PagerResult status = PagerResult::Success;
    PageV2 *page = nullptr;
};

struct PagerAllocateResult {
    PagerResult status = PagerResult::Success;
    int page_num = -1;
    PageV2 *page = nullptr;
    std::vector<PageEffect> effects;
};

struct PagerMutationResult {
    PagerResult status = PagerResult::Success;
    std::vector<PageEffect> effects;
};

struct PagerGetRootResult {
    PagerResult status = PagerResult::Success;
    PageV2 *page = nullptr;
    std::uint64_t root_page_num;
};

class Pager {
	public:
		Pager();
		~Pager();
		PagerResult open(std::string db_file); // TODO: Should also create a btree head page if it doesn't exist. Should configure a constant defining the default order
		PagerGetResult get(int page_num);
        PagerAllocateResult allocate_page(BTreeOperation& operation, V2PageKind kind);
        PagerMutationResult free_page(BTreeOperation& operation, std::uint32_t page_num);
		PagerResult begin_wal_write(
            BTreeOperation& operation,
            std::uint32_t page_num);
		PagerResult ref_page(int page_num); // increment page ref nums. if page.num_refs == 1, call pcache.pin_page(page_num)
		PagerResult unref_page(int page_num); // decrement page ref nums. if page.num_refs == 0 call pcache.unpin_page(int page_num);
        PagerGetRootResult get_btree_root(); //  TODO: should also return root page nuumber.
        PagerMutationResult set_btree_root(BTreeOperation& operation, std::uint32_t root_page_num);
        PagerResult install_page_lsn(
            BTreeOperation& operation,
            std::uint32_t page_num,
            Lsn lsn);
        PagerResult flush_wal_pages();
        void attach_log(Log& log) noexcept { log_ = &log; }
        PageLatchManager& page_latch_manager() noexcept { return page_latch_manager_; }
	private:
		struct PagerReadPageResult {
			PagerResult status = PagerResult::Success;
			PageV2 *page = nullptr;
		};

        std::unordered_set<std::uint32_t> wal_dirty_pages_;

        int db_fd = -1;

		std::string db_name;
		PCache *pCache = nullptr;
		PageLatchManager page_latch_manager_;
		Log *log_ = nullptr;
		mutable std::recursive_mutex mutex_;
		bool is_open = false;
        DBHeader db_header{};

        static constexpr int DB_HEADER_PAGE_NUM = 0;

		void handle_cache_eviction(const PCacheEviction &eviction);
		PagerResult cache_put(PageV2 *page);
        PagerResult mark_loaded_page_wal_pending(
            BTreeOperation& operation,
            PageV2 *page);
        PagerResult flush_wal_page(std::uint32_t page_num);
        PagerResult purge_cache();
        PagerResult create_new_database_file();
        PagerResult load_db_header_from_disk();
        PagerResult ensure_header_page_loaded(PageV2 *&page);
        PagerResult get_or_load_page(int page_num, PageV2 *&page);
        void sync_db_header_to_cached_header_page();
        void read_freelist_links(PageV2 *page, std::uint32_t &next_page_num, std::uint32_t &prev_page_num);
        void write_freelist_links(PageV2 *page, std::uint32_t next_page_num, std::uint32_t prev_page_num);
		PagerReadPageResult read_page_from_disk(int page_num);
};

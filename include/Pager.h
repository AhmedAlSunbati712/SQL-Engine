#pragma once
#include <cstdint>
#include <Page.h>
#include <PageV2.h>
#include <PageLatchManager.h>
#include <Log/WalPayload.h>
#include <unordered_set>
#include <vector>
#include <PCache.h>
#include <Journal.h>
#include <LockMgr.h>

class BTreeOperation;

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
    JournalCreateFailed,
    JournalOpenFailed,
    JournalCorrupt,
    JournalRecordWriteFailed,
    JournalHeaderWriteFailed,
    JournalFlushFailed,
    DbWriteFailed,
    DbFlushFailed,
    JournalTruncateFailed,
    RollbackRecordCorrupt,
    Busy,
    PageNotCached,
    EmptyBTree
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
        PagerAllocateResult allocate_page(V2PageKind kind);
        PagerMutationResult free_page(BTreeOperation& operation, std::uint32_t page_num);
        PagerResult free_page(int page_num);
		PagerResult begin_write(int page_num);
		PagerResult ref_page(int page_num); // increment page ref nums. if page.num_refs == 1, call pcache.pin_page(page_num)
		PagerResult unref_page(int page_num); // decrement page ref nums. if page.num_refs == 0 call pcache.unpin_page(int page_num);
		PagerResult commit_phase_one();
		PagerResult commit_phase_two();
		PagerResult rollback_transaction();
		PagerResult rollback_hot_journal();
        PagerGetRootResult get_btree_root(); //  TODO: should also return root page nuumber. 
        PagerMutationResult set_btree_root(BTreeOperation& operation, std::uint32_t root_page_num);
        PagerResult set_btree_root(std::uint32_t root_page_num);
        PageLatchManager& page_latch_manager() noexcept { return page_latch_manager_; }
	private:
		struct PagerReadPageResult {
			PagerResult status = PagerResult::Success;
			PageV2 *page = nullptr;
		};

		enum class WriteTxnState : std::uint8_t {
			None = 0,
			DirtyInMemory,
			JournalDurable,
		};

		std::unordered_map<int, DirtyPageEntry *> dirty_pages;

        int db_fd = -1;
        int journal_fd = -1;

		std::string db_name;
		std::string jFile_name;
		PCache *pCache = nullptr;
        LockMgr *lock_manager = nullptr;
		PageLatchManager page_latch_manager_;
		bool is_open = false;
		WriteTxnState write_txn_state = WriteTxnState::None;
        DBHeader db_header{};
        DBHeader txn_init_header{};
        bool txn_init_header_valid = false;

        static constexpr int DB_HEADER_PAGE_NUM = 0;
        static constexpr int BUSY_RETRIES = 32;

		void handle_cache_eviction(const PCacheEviction &eviction);
		PagerResult cache_put(PageV2 *page);
		PagerResult journal_has_contents(bool &has_contents);
		PagerResult maybe_recover_hot_journal();
        PagerResult enter_from_nolock(Lock target_lock);
        PagerResult finish_after_clean_state();
        PagerResult restore_lock_after_failure(Lock pre_lock);
        PagerResult replay_hot_journal_under_exclusive();
		PagerResult cleanup_transaction_cache();
        PagerResult purge_cache();
        PagerResult create_new_database_file();
        PagerResult load_db_header_from_disk();
        PagerResult ensure_header_page_loaded(PageV2 *&page);
        PagerResult get_or_load_page(int page_num, PageV2 *&page);
        PagerResult ensure_transaction_started();
        PagerResult mark_header_dirty_for_mutation();
        PagerResult mark_loaded_page_dirty(PageV2 *page);
        void sync_db_header_to_cached_header_page();
        void read_freelist_links(PageV2 *page, std::uint32_t &next_page_num, std::uint32_t &prev_page_num);
        void write_freelist_links(PageV2 *page, std::uint32_t next_page_num, std::uint32_t prev_page_num);
		PagerReadPageResult read_page_from_disk(int page_num);
};

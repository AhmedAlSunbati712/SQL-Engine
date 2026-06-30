#pragma once
#include <cstdint>
#include <Page.h>
#include <unordered_set>
#include <PCache.h>
#include <Journal.h>

enum class PagerResult : std::uint8_t {
    Success = 0,
    DatabaseNotOpen,
    OpenDbFailed,
    DbReadFailed,
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
};

struct PagerGetResult {
    PagerResult status = PagerResult::Success;
    char *data = nullptr;
};

struct PagerAllocateResult {
    PagerResult status = PagerResult::Success;
    int page_num = -1;
    char *data = nullptr;
};

class Pager {
	public:
		Pager();
		~Pager();
		PagerResult open(std::string db_file);
		PagerGetResult get(int page_num);
        PagerAllocateResult allocate_page();
        PagerResult free_page(int page_num);
		PagerResult begin_write(int page_num);
		PagerResult ref_page(int page_num); // increment page ref nums. if page.num_refs == 1, call pcache.pin_page(page_num)
		PagerResult unref_page(int page_num); // decrement page ref nums. if page.num_refs == 0 call pcache.unpin_page(int page_num);
		PagerResult commit_phase_one();
		PagerResult commit_phase_two();
		PagerResult rollback_transaction();
		PagerResult rollback_hot_journal();
	private:
		struct PagerReadPageResult {
			PagerResult status = PagerResult::Success;
			Page *page = nullptr;
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
		bool is_open = false;
		WriteTxnState write_txn_state = WriteTxnState::None;
        DBHeader db_header{};
        DBHeader txn_init_header{};
        bool txn_init_header_valid = false;

        static constexpr int DB_HEADER_PAGE_NUM = 0;

		void handle_cache_eviction(const PCacheEviction &eviction);
		PagerResult cache_put(Page *page);
		PagerResult journal_has_contents(bool &has_contents);
		PagerResult maybe_recover_hot_journal();
		PagerResult cleanup_transaction_cache();
        PagerResult create_new_database_file();
        PagerResult load_db_header_from_disk();
        PagerResult ensure_header_page_loaded(Page *&page);
        PagerResult get_or_load_page(int page_num, Page *&page);
        PagerResult ensure_transaction_started();
        PagerResult mark_header_dirty_for_mutation();
        PagerResult mark_loaded_page_dirty(Page *page);
        void sync_db_header_to_cached_header_page();
        void read_freelist_links(Page *page, std::uint32_t &next_page_num, std::uint32_t &prev_page_num);
        void write_freelist_links(Page *page, std::uint32_t next_page_num, std::uint32_t prev_page_num);
		PagerReadPageResult read_page_from_disk(int page_num);
};

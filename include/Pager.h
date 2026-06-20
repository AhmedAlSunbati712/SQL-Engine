#pragma once
#include <cstdint>
#include <Page.h>
#include <unordered_set>
#include <fstream>
#include <PCache.h>
#include <string.h>
#include <Journal.h>

enum class PagerResult : std::uint8_t {
    Success = 0,
    DatabaseNotOpen,
    OpenDbFailed,
    DbReadFailed,
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

class Pager {
	public:
		Pager();
		~Pager();
		PagerResult open(std::string db_file);
		PagerGetResult get(int page_num);
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

		std::fstream dbFile_handler;

		std::string db_name;
		std::string jFile_name;
		PCache *pCache = nullptr;
		bool is_open = false;
		WriteTxnState write_txn_state = WriteTxnState::None;

		void handle_cache_eviction(const PCacheEviction &eviction);
		PagerResult cache_put(Page *page);
		PagerResult journal_has_contents(bool &has_contents);
		PagerResult maybe_recover_hot_journal();
		PagerResult cleanup_transaction_cache();
		PagerReadPageResult read_page_from_disk(int page_num);
};

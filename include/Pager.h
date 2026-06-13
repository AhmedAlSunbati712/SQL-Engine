#pragma once
#include <Page.h>
#include <unordered_set>
#include <fstream>
#include <PCache.h>
#include <string.h>
#include <Journal.h>



class Pager {
	public:
		Pager(std::string db_file);
		~Pager();
		char *get(int page_num);
		bool begin_write(int page_num);
		void ref_page(int page_num); // increment page ref nums. if page.num_refs == 1, call pcache.pin_page(page_num)
		void unref_page(int page_num); // decrement page ref nums. if page.num_refs == 0 call pcache.unpin_page(int page_num);
		void commit_phase_one();
		void commit_phase_two();
	private:
		std::unordered_map<int, DirtyPageEntry *> dirty_pages;

		std::fstream dbFile_handler;

		std::string db_name;
		std::string jFile_name;
		PCache *pCache;

		Page *read_page_from_disk(int page_num);
};
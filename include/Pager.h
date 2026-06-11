#pragma once
#include <Page.h>
#include <unordered_set>
#include <fstream>
#include <PCache.h>
#include <string.h>


class Pager {
	public:
		Pager(std::string db_file);
		~Pager();
		char *get(int page_num);
		bool begin_write(int page_num);
		void pin_page(int page_num); // if page.num_refs == 0, call pcache.pin_page(page_num). Increment num_refs
		void unpin_page(int page_num); // if page.num_refs == 0, do nothing. Decrement num_refs. If num_refs == 0 call pcache.unpin_page(int page_num);
		bool commit_phase_one();
		bool commit_phase_two();
	private:
		std::unordered_set<Page *> dirty_pages;

		std::fstream dbFile_handler;
		std::fstream jFile_handler;

		std::string db_name;
		std::string jFile_name;
		PCache *pCache;
};
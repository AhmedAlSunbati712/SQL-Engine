#include <gtest/gtest.h>

#include <DBHeaderCodec.h>
#include <Pager.h>
#include <JournalCodec.h>

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

class PagerIntegrationTest : public ::testing::Test {
    protected:
        void SetUp() override {
            auto unique_suffix = std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()
            );
            temp_dir = std::filesystem::temp_directory_path() / ("sqlengine_pager_test_" + unique_suffix);
            db_path = temp_dir / "test.db";
            journal_path = temp_dir / "test.db_journal";
            std::filesystem::create_directories(temp_dir);
        }

        void TearDown() override {
            std::error_code ec;
            std::filesystem::remove_all(temp_dir, ec);
        }

        std::array<char, PAGE_SIZE> make_filled_page(char byte) {
            std::array<char, PAGE_SIZE> page{};
            page.fill(byte);
            return page;
        }

        std::array<char, PAGE_SIZE> read_db_page(int page_num) {
            std::ifstream db_file(db_path, std::ios::binary);
            db_file.seekg(static_cast<std::streamoff>(page_num) * static_cast<std::streamoff>(PAGE_SIZE), std::ios::beg);

            std::array<char, PAGE_SIZE> page{};
            db_file.read(page.data(), static_cast<std::streamsize>(page.size()));
            return page;
        }

        DBHeader read_db_header() {
            std::array<char, PAGE_SIZE> header_page = read_db_page(0);
            DBHeader header{};
            DBHeaderCodec::deserialize_DBHeader(header, header_page.data());
            return header;
        }

        bool journal_exists() const {
            return std::filesystem::exists(journal_path);
        }

        std::uintmax_t journal_size() const {
            if (!std::filesystem::exists(journal_path)) return 0;
            return std::filesystem::file_size(journal_path);
        }

        std::filesystem::path temp_dir;
        std::filesystem::path db_path;
        std::filesystem::path journal_path;
};

TEST_F(PagerIntegrationTest, OpenCreatesNewDatabaseWithHeaderPage) {
    Pager pager;
    ASSERT_EQ(pager.open(db_path.string()), PagerResult::Success);

    ASSERT_TRUE(std::filesystem::exists(db_path));
    EXPECT_EQ(std::filesystem::file_size(db_path), static_cast<std::uintmax_t>(PAGE_SIZE));

    DBHeader header = read_db_header();
    EXPECT_EQ(DBHeaderCodec::validate_DBHeader(header), true);
    EXPECT_EQ(header.db_page_count, 1u);
    EXPECT_EQ(header.freelist_head_page_num, 0u);
    EXPECT_EQ(header.freelist_page_count, 0u);

    PagerGetResult get_result = pager.get(1);
    EXPECT_EQ(get_result.status, PagerResult::PageOutOfRange);
    EXPECT_EQ(get_result.data, nullptr);
}

TEST_F(PagerIntegrationTest, AllocatePageAppendsAndCommitExtendsDatabase) {
    Pager pager;
    ASSERT_EQ(pager.open(db_path.string()), PagerResult::Success);

    PagerAllocateResult allocate_result = pager.allocate_page();
    ASSERT_EQ(allocate_result.status, PagerResult::Success);
    ASSERT_EQ(allocate_result.page_num, 1);
    ASSERT_NE(allocate_result.data, nullptr);

    auto page_bytes = make_filled_page('A');
    std::memcpy(allocate_result.data, page_bytes.data(), PAGE_SIZE);

    ASSERT_EQ(pager.commit_phase_one(), PagerResult::Success);
    ASSERT_EQ(pager.commit_phase_two(), PagerResult::Success);

    EXPECT_EQ(std::filesystem::file_size(db_path), static_cast<std::uintmax_t>(2 * PAGE_SIZE));
    EXPECT_EQ(read_db_page(1), page_bytes);

    DBHeader header = read_db_header();
    EXPECT_EQ(header.db_page_count, 2u);
    EXPECT_EQ(header.freelist_page_count, 0u);
    EXPECT_EQ(journal_exists(), true);
    EXPECT_EQ(journal_size(), 0u);
}

TEST_F(PagerIntegrationTest, RollbackTransactionRemovesAppendedPages) {
    Pager pager;
    ASSERT_EQ(pager.open(db_path.string()), PagerResult::Success);

    PagerAllocateResult allocate_result = pager.allocate_page();
    ASSERT_EQ(allocate_result.status, PagerResult::Success);
    ASSERT_EQ(allocate_result.page_num, 1);

    auto page_bytes = make_filled_page('B');
    std::memcpy(allocate_result.data, page_bytes.data(), PAGE_SIZE);

    ASSERT_EQ(pager.rollback_transaction(), PagerResult::Success);

    EXPECT_EQ(std::filesystem::file_size(db_path), static_cast<std::uintmax_t>(PAGE_SIZE));

    DBHeader header = read_db_header();
    EXPECT_EQ(header.db_page_count, 1u);

    PagerGetResult get_result = pager.get(1);
    EXPECT_EQ(get_result.status, PagerResult::PageOutOfRange);
    EXPECT_EQ(get_result.data, nullptr);
}

TEST_F(PagerIntegrationTest, FreePageAddsToFreelistAndNextAllocationReusesIt) {
    {
        Pager pager;
        ASSERT_EQ(pager.open(db_path.string()), PagerResult::Success);

        PagerAllocateResult allocate_result = pager.allocate_page();
        ASSERT_EQ(allocate_result.status, PagerResult::Success);
        ASSERT_EQ(allocate_result.page_num, 1);

        auto page_bytes = make_filled_page('C');
        std::memcpy(allocate_result.data, page_bytes.data(), PAGE_SIZE);

        ASSERT_EQ(pager.commit_phase_one(), PagerResult::Success);
        ASSERT_EQ(pager.commit_phase_two(), PagerResult::Success);

        ASSERT_EQ(pager.free_page(1), PagerResult::Success);
        ASSERT_EQ(pager.unref_page(1), PagerResult::Success);
        ASSERT_EQ(pager.commit_phase_one(), PagerResult::Success);
        ASSERT_EQ(pager.commit_phase_two(), PagerResult::Success);
    }

    DBHeader header_after_free = read_db_header();
    EXPECT_EQ(header_after_free.db_page_count, 2u);
    EXPECT_EQ(header_after_free.freelist_head_page_num, 1u);
    EXPECT_EQ(header_after_free.freelist_page_count, 1u);

    Pager pager;
    ASSERT_EQ(pager.open(db_path.string()), PagerResult::Success);

    PagerAllocateResult reuse_result = pager.allocate_page();
    ASSERT_EQ(reuse_result.status, PagerResult::Success);
    ASSERT_EQ(reuse_result.page_num, 1);

    std::array<char, PAGE_SIZE> zero_page{};
    EXPECT_EQ(std::memcmp(reuse_result.data, zero_page.data(), PAGE_SIZE), 0);
}

TEST_F(PagerIntegrationTest, OpenRecoversHotJournalAndTruncatesAppendedPages) {
    auto page_bytes = make_filled_page('D');

    {
        Pager writer_pager;
        ASSERT_EQ(writer_pager.open(db_path.string()), PagerResult::Success);

        PagerAllocateResult allocate_result = writer_pager.allocate_page();
        ASSERT_EQ(allocate_result.status, PagerResult::Success);
        ASSERT_EQ(allocate_result.page_num, 1);

        std::memcpy(allocate_result.data, page_bytes.data(), PAGE_SIZE);

        ASSERT_EQ(writer_pager.commit_phase_one(), PagerResult::Success);
        EXPECT_TRUE(journal_exists());
        EXPECT_GT(journal_size(), 0u);
    }

    EXPECT_EQ(std::filesystem::file_size(db_path), static_cast<std::uintmax_t>(2 * PAGE_SIZE));

    Pager recovery_pager;
    ASSERT_EQ(recovery_pager.open(db_path.string()), PagerResult::Success);

    DBHeader header = read_db_header();
    EXPECT_EQ(header.db_page_count, 1u);
    EXPECT_EQ(std::filesystem::file_size(db_path), static_cast<std::uintmax_t>(PAGE_SIZE));
    EXPECT_TRUE(journal_exists());
    EXPECT_EQ(journal_size(), 0u);

    PagerGetResult get_result = recovery_pager.get(1);
    EXPECT_EQ(get_result.status, PagerResult::PageOutOfRange);
    EXPECT_EQ(get_result.data, nullptr);
}

TEST_F(PagerIntegrationTest, BeginWriteOnExistingPageCommitsUpdatedBytes) {
    Pager writer_pager;
    ASSERT_EQ(writer_pager.open(db_path.string()), PagerResult::Success);
    PagerAllocateResult allocate_result = writer_pager.allocate_page();
    ASSERT_EQ(allocate_result.status, PagerResult::Success);
    EXPECT_EQ(allocate_result.page_num, 1);

    auto page_bytes = make_filled_page('A');
    std::memcpy(allocate_result.data, page_bytes.data(), PAGE_SIZE);
    writer_pager.commit_phase_one();

    // Make sure that there are not journal records on disk for this.
    std::fstream jFile;
    jFile.open(journal_path, std::ios::in | std::ios::binary);
    char jHeader_bytes[JOURNAL_HEADER_SIZE];
    jFile.read(jHeader_bytes, JOURNAL_HEADER_SIZE);
    JournalHeader jHeader;
    Journal::deserialize_jHeader(jHeader, jHeader_bytes);
    EXPECT_EQ(jHeader.init_db_page_count, 1);
    EXPECT_EQ(jHeader.page_count, 1);
    EXPECT_EQ(std::filesystem::file_size(journal_path), PAGE_SIZE + JOURNAL_PAGE_RECORD);

    jFile.close();

    writer_pager.commit_phase_two();
    ASSERT_EQ(std::filesystem::is_empty(journal_path), true);

    DBHeader header = read_db_header();
    EXPECT_EQ(header.db_page_count, 2);
    EXPECT_EQ(header.file_change_counter, 1);

    // Now, we need to get that page, and ensure it has the content we wrote!
    PagerGetResult get_result = writer_pager.get(1);
    ASSERT_EQ(get_result.status, PagerResult::Success);
    char *get_page_bytes = get_result.data;
    for (int i = 0; i < PAGE_SIZE; i++) {
        EXPECT_EQ(get_page_bytes[i], 'A');
    }

    header = read_db_header();
    char header_bytes[PAGE_SIZE] = {};
    DBHeaderCodec::serialize_DBHeader(header, header_bytes);
    PagerResult begin_write_result = writer_pager.begin_write(1);
    ASSERT_EQ(begin_write_result, PagerResult::Success);
    for (int i = 0; i < PAGE_SIZE; i++) {
        get_page_bytes[i] = 'B';
    }
    writer_pager.commit_phase_one();

    // Validate the journal has expected data
    jFile.open(journal_path, std::ios::in | std::ios::binary);
    jFile.read(jHeader_bytes, JOURNAL_HEADER_SIZE);
    Journal::deserialize_jHeader(jHeader, jHeader_bytes);
    EXPECT_EQ(jHeader.init_db_page_count, 2);
    EXPECT_EQ(jHeader.page_count, 2);
    EXPECT_EQ(std::filesystem::file_size(journal_path), PAGE_SIZE + JOURNAL_PAGE_RECORD * 2);

    // The page records
    jFile.seekg(static_cast<std::streamoff>(PAGE_SIZE));
    char curr_page_record[JOURNAL_PAGE_RECORD];
    for (int i = 0; i < 2; i++) {
        jFile.read(curr_page_record, JOURNAL_PAGE_RECORD);
        JournalPageRecord jPage_record;
        Journal::deserialize_jPage_record(jPage_record, curr_page_record);
        for (int j = 0; j < PAGE_SIZE; j++) {
            if (jPage_record.page_num == 0) {
                EXPECT_EQ(jPage_record.data[j], header_bytes[j]);
            } else {
                EXPECT_EQ(jPage_record.data[j], 'A');
            }
        }

    }

    jFile.close();

    ASSERT_EQ(writer_pager.commit_phase_two(), PagerResult::Success);
    EXPECT_TRUE(std::filesystem::is_empty(journal_path));

    auto updated_page_bytes = make_filled_page('B');
    EXPECT_EQ(read_db_page(1), updated_page_bytes);

    header = read_db_header();
    EXPECT_EQ(header.db_page_count, 2u);
    EXPECT_EQ(header.file_change_counter, 2u);
    EXPECT_EQ(header.freelist_head_page_num, 0u);
    EXPECT_EQ(header.freelist_page_count, 0u);

    Pager recovery_pager;
    ASSERT_EQ(recovery_pager.open(db_path.string()), PagerResult::Success);

    PagerGetResult recovery_get_result = recovery_pager.get(1);
    ASSERT_EQ(recovery_get_result.status, PagerResult::Success);
    for (int i = 0; i < PAGE_SIZE; i++) {
        EXPECT_EQ(recovery_get_result.data[i], 'B');
    }

}

} // namespace

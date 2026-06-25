#include <gtest/gtest.h>

#include <DBHeaderCodec.h>
#include <Pager.h>

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

} // namespace

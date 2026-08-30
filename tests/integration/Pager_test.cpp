#include <gtest/gtest.h>

#include <DBHeaderCodec.h>
#include <Endian.h>
#include <Log/Log.h>
#include <Log/WalRecords.h>
#include <Pager.h>
#include <V2PageCodec.h>
#include <containers/BTreeOperation.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <system_error>

namespace {

Config wal_config() {
    return {
        .max_index_bytes = 1000 * Index::ENTRY_SIZE,
        .max_store_bytes = 16 * 1024 * 1024,
        .initial_lsn = 1,
    };
}

class PagerIntegrationTest : public ::testing::Test {
    protected:
        void SetUp() override {
            auto unique_suffix = std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()
            );
            temp_dir = std::filesystem::temp_directory_path() / ("stoneleafdb_pager_test_" + unique_suffix);
            db_path = temp_dir / "test.db";
            wal_path = temp_dir / "test.wal";
            std::filesystem::create_directories(temp_dir);

            log = std::make_unique<Log>(wal_config());
            log->open(wal_path.string());
        }

        void TearDown() override {
            std::error_code ec;
            std::filesystem::remove_all(temp_dir, ec);
        }

        // Pager only implements the WAL mutation path, so every real test
        // attaches this fixture's Log before open().
        PagerResult open_pager(Pager &pager) {
            pager.attach_log(*log);
            return pager.open(db_path.string());
        }

        // install_page_lsn requires an LSN the Log actually knows about, since
        // flush_wal_page later calls Log::sync_through(lsn). Appending a
        // trivial record is the simplest way to reserve one for a test.
        Lsn next_wal_lsn() {
            return log->append(WalRecords::begin(1));
        }

        std::array<char, PAGE_SIZE> make_filled_page(
            char byte,
            std::uint32_t page_num = 1) {
            std::array<char, PAGE_SIZE> page{};
            V2PageCodec::initialize(
                page,
                page_num,
                V2PageKind::BTreeLeaf);
            std::fill(
                page.begin() + V2_PAGE_HEADER_SIZE,
                page.end(),
                byte);
            V2PageCodec::update_checksum(page);
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
            DBHeaderCodec::deserialize_DBHeader(
                header,
                header_page.data() + V2_PAGE_HEADER_SIZE);
            return header;
        }

        void expect_zero_payload(const PageV2 *page) {
            ASSERT_NE(page, nullptr);
            for (std::size_t offset = V2_PAGE_HEADER_SIZE;
                 offset < V2_PAGE_SIZE;
                 ++offset) {
                EXPECT_EQ(page->data[offset], '\0');
            }
        }

        std::unique_ptr<Log> log;
        std::filesystem::path temp_dir;
        std::filesystem::path db_path;
        std::filesystem::path wal_path;
};

} // namespace

TEST_F(PagerIntegrationTest, OpenFailsWithoutAttachedLog) {
    Pager pager;
    EXPECT_EQ(pager.open(db_path.string()), PagerResult::LogNotAttached);
}

TEST_F(PagerIntegrationTest, OpenCreatesNewDatabaseWithHeaderPage) {
    Pager pager;
    ASSERT_EQ(open_pager(pager), PagerResult::Success);

    ASSERT_TRUE(std::filesystem::exists(db_path));
    EXPECT_EQ(std::filesystem::file_size(db_path), static_cast<std::uintmax_t>(PAGE_SIZE));

    const std::array<char, PAGE_SIZE> header_page = read_db_page(0);
    EXPECT_EQ(
        V2PageCodec::validate(header_page),
        V2PageCodecResult::Success);
    EXPECT_EQ(V2PageCodec::page_num(header_page), 0u);
    EXPECT_EQ(
        V2PageCodec::page_kind(header_page),
        V2PageKind::DatabaseMetadata);

    DBHeader header = read_db_header();
    EXPECT_EQ(DBHeaderCodec::validate_DBHeader(header), true);
    EXPECT_EQ(header.db_page_count, 1u);
    EXPECT_EQ(header.freelist_head_page_num, 0u);
    EXPECT_EQ(header.freelist_page_count, 0u);

    PagerGetResult get_result = pager.get(1);
    EXPECT_EQ(get_result.status, PagerResult::PageOutOfRange);
    EXPECT_EQ(get_result.page, nullptr);
}

TEST_F(PagerIntegrationTest, OperationAwareAllocationRetainsLatchesAndReturnsEffects) {
    Pager pager;
    ASSERT_EQ(open_pager(pager), PagerResult::Success);

    BTreeOperation operation(pager.page_latch_manager());
    PagerAllocateResult allocation = pager.allocate_page(
        operation,
        V2PageKind::BTreeLeaf);

    ASSERT_EQ(allocation.status, PagerResult::Success);
    ASSERT_EQ(allocation.page_num, 1);
    ASSERT_EQ(allocation.effects.size(), 2U);
    EXPECT_EQ(allocation.effects[0].kind, PageEffectKind::Write);
    EXPECT_EQ(allocation.effects[0].page_num, 0U);
    EXPECT_EQ(allocation.effects[1].kind, PageEffectKind::Allocate);
    EXPECT_EQ(allocation.effects[1].page_num, 1U);
    EXPECT_EQ(allocation.effects[1].after_image, allocation.page->data);

    EXPECT_EQ(operation.latch_mode(0), PageLatchMode::Exclusive);
    EXPECT_EQ(operation.latch_mode(1), PageLatchMode::Exclusive);
    ASSERT_EQ(pager.unref_page(1), PagerResult::Success);

    std::future<PageReadLatch> blocked_reader = std::async(
        std::launch::async,
        [&] { return pager.page_latch_manager().lock_shared(1); });
    EXPECT_EQ(blocked_reader.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);

    operation.release_all();
    EXPECT_EQ(blocked_reader.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_TRUE(blocked_reader.get().owns_lock());
}

TEST_F(PagerIntegrationTest, OperationAwareFreeReturnsEveryChangedPage) {
    Pager pager;
    ASSERT_EQ(open_pager(pager), PagerResult::Success);

    BTreeOperation setup_operation(pager.page_latch_manager());
    PagerAllocateResult first = pager.allocate_page(setup_operation, V2PageKind::BTreeLeaf);
    ASSERT_EQ(first.status, PagerResult::Success);
    PagerAllocateResult second = pager.allocate_page(setup_operation, V2PageKind::BTreeLeaf);
    ASSERT_EQ(second.status, PagerResult::Success);
    Lsn setup_lsn = next_wal_lsn();
    ASSERT_EQ(pager.install_page_lsn(setup_operation, 0, setup_lsn), PagerResult::Success);
    ASSERT_EQ(pager.install_page_lsn(setup_operation, 1, setup_lsn), PagerResult::Success);
    ASSERT_EQ(pager.install_page_lsn(setup_operation, 2, setup_lsn), PagerResult::Success);
    setup_operation.release_all();
    ASSERT_EQ(pager.flush_wal_pages(), PagerResult::Success);
    ASSERT_EQ(pager.unref_page(1), PagerResult::Success);
    ASSERT_EQ(pager.unref_page(2), PagerResult::Success);

    BTreeOperation free_setup_operation(pager.page_latch_manager());
    PagerMutationResult first_free = pager.free_page(free_setup_operation, 1);
    ASSERT_EQ(first_free.status, PagerResult::Success);
    Lsn free_setup_lsn = next_wal_lsn();
    ASSERT_EQ(pager.install_page_lsn(free_setup_operation, 0, free_setup_lsn), PagerResult::Success);
    ASSERT_EQ(pager.install_page_lsn(free_setup_operation, 1, free_setup_lsn), PagerResult::Success);
    free_setup_operation.release_all();
    ASSERT_EQ(pager.flush_wal_pages(), PagerResult::Success);

    BTreeOperation operation(pager.page_latch_manager());
    PagerMutationResult freed = pager.free_page(operation, 2);

    ASSERT_EQ(freed.status, PagerResult::Success);
    ASSERT_EQ(freed.effects.size(), 3U);
    EXPECT_EQ(freed.effects[0].page_num, 0U);
    EXPECT_EQ(freed.effects[0].kind, PageEffectKind::Write);
    EXPECT_EQ(freed.effects[1].page_num, 1U);
    EXPECT_EQ(freed.effects[1].kind, PageEffectKind::Write);
    EXPECT_EQ(freed.effects[2].page_num, 2U);
    EXPECT_EQ(freed.effects[2].kind, PageEffectKind::Free);

    EXPECT_EQ(operation.latch_mode(0), PageLatchMode::Exclusive);
    EXPECT_EQ(operation.latch_mode(1), PageLatchMode::Exclusive);
    EXPECT_EQ(operation.latch_mode(2), PageLatchMode::Exclusive);
    operation.release_all();
}

TEST_F(PagerIntegrationTest, AllocatePageAppendsAndCommitExtendsDatabase) {
    Pager pager;
    ASSERT_EQ(open_pager(pager), PagerResult::Success);

    BTreeOperation operation(pager.page_latch_manager());
    PagerAllocateResult allocate_result = pager.allocate_page(operation, V2PageKind::BTreeLeaf);
    ASSERT_EQ(allocate_result.status, PagerResult::Success);
    ASSERT_EQ(allocate_result.page_num, 1);
    ASSERT_NE(allocate_result.page, nullptr);
    EXPECT_EQ(allocate_result.page->page_num, 1u);
    EXPECT_EQ(
        V2PageCodec::page_num(allocate_result.page->data),
        allocate_result.page->page_num);
    EXPECT_EQ(
        V2PageCodec::page_kind(allocate_result.page->data),
        V2PageKind::BTreeLeaf);

    auto page_bytes = make_filled_page('A');
    std::memcpy(allocate_result.page->data.data(), page_bytes.data(), PAGE_SIZE);

    Lsn lsn = next_wal_lsn();
    ASSERT_EQ(pager.install_page_lsn(operation, 0, lsn), PagerResult::Success);
    ASSERT_EQ(pager.install_page_lsn(operation, 1, lsn), PagerResult::Success);
    // install_page_lsn stamps the assigned LSN and recomputes the checksum in
    // place, so the persisted bytes differ from page_bytes at those fields.
    page_bytes = allocate_result.page->data;
    operation.release_all();
    ASSERT_EQ(pager.flush_wal_pages(), PagerResult::Success);

    EXPECT_EQ(std::filesystem::file_size(db_path), static_cast<std::uintmax_t>(2 * PAGE_SIZE));
    EXPECT_EQ(read_db_page(1), page_bytes);

    DBHeader header = read_db_header();
    EXPECT_EQ(header.db_page_count, 2u);
    EXPECT_EQ(header.freelist_page_count, 0u);
}

TEST_F(PagerIntegrationTest, AllocatePageRejectsNonTreeKinds) {
    Pager pager;
    ASSERT_EQ(open_pager(pager), PagerResult::Success);

    BTreeOperation operation(pager.page_latch_manager());
    EXPECT_EQ(
        pager.allocate_page(operation, V2PageKind::DatabaseMetadata).status,
        PagerResult::InvalidPageKind);
    EXPECT_EQ(
        pager.allocate_page(operation, V2PageKind::Freelist).status,
        PagerResult::InvalidPageKind);
}

TEST_F(PagerIntegrationTest, GetRejectsCorruptV2Page) {
    {
        Pager pager;
        ASSERT_EQ(open_pager(pager), PagerResult::Success);
        BTreeOperation operation(pager.page_latch_manager());
        PagerAllocateResult allocation = pager.allocate_page(operation, V2PageKind::BTreeLeaf);
        ASSERT_EQ(allocation.status, PagerResult::Success);
        Lsn lsn = next_wal_lsn();
        ASSERT_EQ(pager.install_page_lsn(operation, 0, lsn), PagerResult::Success);
        ASSERT_EQ(pager.install_page_lsn(operation, 1, lsn), PagerResult::Success);
        operation.release_all();
        ASSERT_EQ(pager.flush_wal_pages(), PagerResult::Success);
    }

    std::fstream database(db_path, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(database.is_open());
    database.seekp(
        static_cast<std::streamoff>(PAGE_SIZE + V2_PAGE_HEADER_SIZE),
        std::ios::beg);
    const char corrupt = static_cast<char>(0xA5);
    database.write(&corrupt, 1);
    database.close();

    Pager pager;
    ASSERT_EQ(open_pager(pager), PagerResult::Success);
    EXPECT_EQ(pager.get(1).status, PagerResult::PageCorrupt);
}

TEST_F(PagerIntegrationTest, GetRejectsEncodedPageNumberMismatch) {
    {
        Pager pager;
        ASSERT_EQ(open_pager(pager), PagerResult::Success);
        BTreeOperation operation(pager.page_latch_manager());
        PagerAllocateResult allocation = pager.allocate_page(operation, V2PageKind::BTreeLeaf);
        ASSERT_EQ(allocation.status, PagerResult::Success);
        Lsn lsn = next_wal_lsn();
        ASSERT_EQ(pager.install_page_lsn(operation, 0, lsn), PagerResult::Success);
        ASSERT_EQ(pager.install_page_lsn(operation, 1, lsn), PagerResult::Success);
        operation.release_all();
        ASSERT_EQ(pager.flush_wal_pages(), PagerResult::Success);
    }

    std::array<char, PAGE_SIZE> page = read_db_page(1);
    put_u32_be(page.data() + PAGE_NUM_OFFSET, 7);
    V2PageCodec::update_checksum(page);

    std::fstream database(db_path, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(database.is_open());
    database.seekp(static_cast<std::streamoff>(PAGE_SIZE), std::ios::beg);
    database.write(page.data(), static_cast<std::streamsize>(page.size()));
    database.close();

    Pager pager;
    ASSERT_EQ(open_pager(pager), PagerResult::Success);
    EXPECT_EQ(pager.get(1).status, PagerResult::PageCorrupt);
}

TEST_F(PagerIntegrationTest, FreePageAddsToFreelistAndNextAllocationReusesIt) {
    {
        Pager pager;
        ASSERT_EQ(open_pager(pager), PagerResult::Success);

        BTreeOperation allocate_operation(pager.page_latch_manager());
        PagerAllocateResult allocate_result = pager.allocate_page(allocate_operation, V2PageKind::BTreeLeaf);
        ASSERT_EQ(allocate_result.status, PagerResult::Success);
        ASSERT_EQ(allocate_result.page_num, 1);

        auto page_bytes = make_filled_page('C');
        std::memcpy(allocate_result.page->data.data(), page_bytes.data(), PAGE_SIZE);

        Lsn allocate_lsn = next_wal_lsn();
        ASSERT_EQ(pager.install_page_lsn(allocate_operation, 0, allocate_lsn), PagerResult::Success);
        ASSERT_EQ(pager.install_page_lsn(allocate_operation, 1, allocate_lsn), PagerResult::Success);
        allocate_operation.release_all();
        ASSERT_EQ(pager.flush_wal_pages(), PagerResult::Success);
        ASSERT_EQ(pager.unref_page(1), PagerResult::Success);

        BTreeOperation free_operation(pager.page_latch_manager());
        ASSERT_EQ(pager.free_page(free_operation, 1).status, PagerResult::Success);
        Lsn free_lsn = next_wal_lsn();
        ASSERT_EQ(pager.install_page_lsn(free_operation, 0, free_lsn), PagerResult::Success);
        ASSERT_EQ(pager.install_page_lsn(free_operation, 1, free_lsn), PagerResult::Success);
        free_operation.release_all();
        ASSERT_EQ(pager.flush_wal_pages(), PagerResult::Success);
    }

    DBHeader header_after_free = read_db_header();
    EXPECT_EQ(header_after_free.db_page_count, 2u);
    EXPECT_EQ(header_after_free.freelist_head_page_num, 1u);
    EXPECT_EQ(header_after_free.freelist_page_count, 1u);

    Pager pager;
    ASSERT_EQ(open_pager(pager), PagerResult::Success);

    BTreeOperation reuse_operation(pager.page_latch_manager());
    PagerAllocateResult reuse_result = pager.allocate_page(reuse_operation, V2PageKind::BTreeLeaf);
    ASSERT_EQ(reuse_result.status, PagerResult::Success);
    ASSERT_EQ(reuse_result.page_num, 1);

    expect_zero_payload(reuse_result.page);
}

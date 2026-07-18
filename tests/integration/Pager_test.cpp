#include <gtest/gtest.h>

#include <DBHeaderCodec.h>
#include <Pager.h>
#include <JournalCodec.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <sys/wait.h>
#include <unistd.h>

namespace {

class PagerIntegrationTest : public ::testing::Test {
    protected:
        void SetUp() override {
            auto unique_suffix = std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()
            );
            temp_dir = std::filesystem::temp_directory_path() / ("stoneleafdb_pager_test_" + unique_suffix);
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

        void write_exact_fd(int fd, const void *buffer, std::size_t size) {
            const char *bytes = static_cast<const char *>(buffer);
            std::size_t written = 0;
            while (written < size) {
                ssize_t rc = ::write(fd, bytes + written, size - written);
                if (rc == -1) continue;
                written += static_cast<std::size_t>(rc);
            }
        }

        void read_exact_fd(int fd, void *buffer, std::size_t size) {
            char *bytes = static_cast<char *>(buffer);
            std::size_t read_bytes = 0;
            while (read_bytes < size) {
                ssize_t rc = ::read(fd, bytes + read_bytes, size - read_bytes);
                if (rc == -1) continue;
                read_bytes += static_cast<std::size_t>(rc);
            }
        }

        void send_pager_result(int fd, PagerResult result) {
            auto raw = static_cast<std::uint8_t>(result);
            write_exact_fd(fd, &raw, sizeof(raw));
        }

        PagerResult recv_pager_result(int fd) {
            std::uint8_t raw = 0;
            read_exact_fd(fd, &raw, sizeof(raw));
            return static_cast<PagerResult>(raw);
        }

        void send_signal(int fd) {
            char signal = 'x';
            write_exact_fd(fd, &signal, sizeof(signal));
        }

        void wait_signal(int fd) {
            char signal = '\0';
            read_exact_fd(fd, &signal, sizeof(signal));
        }

        void expect_child_exit_ok(pid_t pid) {
            int status = 0;
            ASSERT_EQ(::waitpid(pid, &status, 0), pid);
            ASSERT_TRUE(WIFEXITED(status));
            EXPECT_EQ(WEXITSTATUS(status), 0);
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

TEST_F(PagerIntegrationTest, GetFromNoLockSeesPageCountAfterAnotherProcessAppends) {
    Pager stale_reader_pager;
    ASSERT_EQ(stale_reader_pager.open(db_path.string()), PagerResult::Success);

    {
        Pager writer_pager;
        ASSERT_EQ(writer_pager.open(db_path.string()), PagerResult::Success);

        PagerAllocateResult allocate_result = writer_pager.allocate_page();
        ASSERT_EQ(allocate_result.status, PagerResult::Success);
        ASSERT_EQ(allocate_result.page_num, 1);

        auto page_bytes = make_filled_page('Z');
        std::memcpy(allocate_result.data, page_bytes.data(), PAGE_SIZE);

        ASSERT_EQ(writer_pager.commit_phase_one(), PagerResult::Success);
        ASSERT_EQ(writer_pager.commit_phase_two(), PagerResult::Success);
    }

    // stale_reader_pager still has the old header snapshot from open() where db_page_count was 1.
    // get() now has to refresh under SHARED before it decides this page is out of range.
    PagerGetResult get_result = stale_reader_pager.get(1);
    ASSERT_EQ(get_result.status, PagerResult::Success);
    for (int i = 0; i < PAGE_SIZE; i++) {
        EXPECT_EQ(get_result.data[i], 'Z');
    }
}

TEST_F(PagerIntegrationTest, RefPageFromNoLockReacquiresShared) {
    Pager pager;
    ASSERT_EQ(pager.open(db_path.string()), PagerResult::Success);

    PagerAllocateResult allocate_result = pager.allocate_page();
    ASSERT_EQ(allocate_result.status, PagerResult::Success);
    ASSERT_EQ(allocate_result.page_num, 1);

    auto page_bytes = make_filled_page('R');
    std::memcpy(allocate_result.data, page_bytes.data(), PAGE_SIZE);

    ASSERT_EQ(pager.commit_phase_one(), PagerResult::Success);
    ASSERT_EQ(pager.commit_phase_two(), PagerResult::Success);
    ASSERT_EQ(pager.unref_page(1), PagerResult::Success);

    ASSERT_EQ(pager.ref_page(1), PagerResult::Success);
    ASSERT_EQ(pager.unref_page(1), PagerResult::Success);

    PagerGetResult get_result = pager.get(1);
    ASSERT_EQ(get_result.status, PagerResult::Success);
    for (int i = 0; i < PAGE_SIZE; i++) {
        EXPECT_EQ(get_result.data[i], 'R');
    }
}

TEST_F(PagerIntegrationTest, RefPageFromNoLockReturnsPageNotCachedAfterPurge) {
    auto original_page_bytes = make_filled_page('S');
    auto updated_page_bytes = make_filled_page('T');

    {
        Pager setup_pager;
        ASSERT_EQ(setup_pager.open(db_path.string()), PagerResult::Success);

        PagerAllocateResult allocate_result = setup_pager.allocate_page();
        ASSERT_EQ(allocate_result.status, PagerResult::Success);
        ASSERT_EQ(allocate_result.page_num, 1);
        std::memcpy(allocate_result.data, original_page_bytes.data(), PAGE_SIZE);

        ASSERT_EQ(setup_pager.commit_phase_one(), PagerResult::Success);
        ASSERT_EQ(setup_pager.commit_phase_two(), PagerResult::Success);
    }

    Pager stale_reader_pager;
    ASSERT_EQ(stale_reader_pager.open(db_path.string()), PagerResult::Success);

    PagerGetResult stale_get_result = stale_reader_pager.get(1);
    ASSERT_EQ(stale_get_result.status, PagerResult::Success);
    ASSERT_EQ(stale_reader_pager.unref_page(1), PagerResult::Success);

    {
        Pager writer_pager;
        ASSERT_EQ(writer_pager.open(db_path.string()), PagerResult::Success);

        PagerGetResult writer_get_result = writer_pager.get(1);
        ASSERT_EQ(writer_get_result.status, PagerResult::Success);
        ASSERT_EQ(writer_pager.begin_write(1), PagerResult::Success);
        std::memcpy(writer_get_result.data, updated_page_bytes.data(), PAGE_SIZE);

        ASSERT_EQ(writer_pager.commit_phase_one(), PagerResult::Success);
        ASSERT_EQ(writer_pager.commit_phase_two(), PagerResult::Success);
    }

    // ref_page() is still cache-only. If refresh from NOLOCK purges stale pages, it should not
    // silently reload from disk and pretend the cached page survived.
    ASSERT_EQ(stale_reader_pager.ref_page(1), PagerResult::PageNotCached);

    PagerGetResult refreshed_get_result = stale_reader_pager.get(1);
    ASSERT_EQ(refreshed_get_result.status, PagerResult::Success);
    for (int i = 0; i < PAGE_SIZE; i++) {
        EXPECT_EQ(refreshed_get_result.data[i], 'T');
    }
}

TEST_F(PagerIntegrationTest, MultipleProcessesCanReadSamePageConcurrently) {
    {
        Pager setup_pager;
        ASSERT_EQ(setup_pager.open(db_path.string()), PagerResult::Success);

        PagerAllocateResult allocate_result = setup_pager.allocate_page();
        ASSERT_EQ(allocate_result.status, PagerResult::Success);
        ASSERT_EQ(allocate_result.page_num, 1);

        auto page_bytes = make_filled_page('U');
        std::memcpy(allocate_result.data, page_bytes.data(), PAGE_SIZE);

        ASSERT_EQ(setup_pager.commit_phase_one(), PagerResult::Success);
        ASSERT_EQ(setup_pager.commit_phase_two(), PagerResult::Success);
    }

    int child_to_parent[2];
    int parent_to_child[2];
    ASSERT_EQ(::pipe(child_to_parent), 0);
    ASSERT_EQ(::pipe(parent_to_child), 0);

    pid_t child_pid = ::fork();
    ASSERT_NE(child_pid, -1);

    if (child_pid == 0) {
        ::close(child_to_parent[0]);
        ::close(parent_to_child[1]);

        Pager child_pager;
        send_pager_result(child_to_parent[1], child_pager.open(db_path.string()));
        PagerGetResult child_get_result = child_pager.get(1);
        send_pager_result(child_to_parent[1], child_get_result.status);
        wait_signal(parent_to_child[0]);
        child_pager.unref_page(1);

        ::close(child_to_parent[1]);
        ::close(parent_to_child[0]);
        _exit(0);
    }

    ::close(child_to_parent[1]);
    ::close(parent_to_child[0]);

    ASSERT_EQ(recv_pager_result(child_to_parent[0]), PagerResult::Success);
    ASSERT_EQ(recv_pager_result(child_to_parent[0]), PagerResult::Success);

    Pager parent_pager;
    ASSERT_EQ(parent_pager.open(db_path.string()), PagerResult::Success);
    PagerGetResult parent_get_result = parent_pager.get(1);
    ASSERT_EQ(parent_get_result.status, PagerResult::Success);
    for (int i = 0; i < PAGE_SIZE; i++) {
        EXPECT_EQ(parent_get_result.data[i], 'U');
    }

    send_signal(parent_to_child[1]);
    ASSERT_EQ(parent_pager.unref_page(1), PagerResult::Success);

    ::close(child_to_parent[0]);
    ::close(parent_to_child[1]);
    expect_child_exit_ok(child_pid);
}

TEST_F(PagerIntegrationTest, SecondWriterReturnsBusyWhileReservedHeldByAnotherProcess) {
    int child_to_parent[2];
    int parent_to_child[2];
    ASSERT_EQ(::pipe(child_to_parent), 0);
    ASSERT_EQ(::pipe(parent_to_child), 0);

    pid_t child_pid = ::fork();
    ASSERT_NE(child_pid, -1);

    if (child_pid == 0) {
        ::close(child_to_parent[0]);
        ::close(parent_to_child[1]);

        Pager child_pager;
        send_pager_result(child_to_parent[1], child_pager.open(db_path.string()));
        PagerAllocateResult child_allocate_result = child_pager.allocate_page();
        send_pager_result(child_to_parent[1], child_allocate_result.status);
        wait_signal(parent_to_child[0]);
        child_pager.rollback_transaction();

        ::close(child_to_parent[1]);
        ::close(parent_to_child[0]);
        _exit(0);
    }

    ::close(child_to_parent[1]);
    ::close(parent_to_child[0]);

    ASSERT_EQ(recv_pager_result(child_to_parent[0]), PagerResult::Success);
    ASSERT_EQ(recv_pager_result(child_to_parent[0]), PagerResult::Success);

    Pager parent_pager;
    ASSERT_EQ(parent_pager.open(db_path.string()), PagerResult::Success);
    PagerAllocateResult parent_allocate_result = parent_pager.allocate_page();
    ASSERT_EQ(parent_allocate_result.status, PagerResult::Busy);

    send_signal(parent_to_child[1]);
    ::close(child_to_parent[0]);
    ::close(parent_to_child[1]);
    expect_child_exit_ok(child_pid);
}

TEST_F(PagerIntegrationTest, CommitPhaseOneReturnsBusyWhileAnotherProcessStillReads) {
    {
        Pager setup_pager;
        ASSERT_EQ(setup_pager.open(db_path.string()), PagerResult::Success);

        PagerAllocateResult allocate_result = setup_pager.allocate_page();
        ASSERT_EQ(allocate_result.status, PagerResult::Success);
        ASSERT_EQ(allocate_result.page_num, 1);

        auto page_bytes = make_filled_page('V');
        std::memcpy(allocate_result.data, page_bytes.data(), PAGE_SIZE);

        ASSERT_EQ(setup_pager.commit_phase_one(), PagerResult::Success);
        ASSERT_EQ(setup_pager.commit_phase_two(), PagerResult::Success);
    }

    int child_to_parent[2];
    int parent_to_child[2];
    ASSERT_EQ(::pipe(child_to_parent), 0);
    ASSERT_EQ(::pipe(parent_to_child), 0);

    pid_t child_pid = ::fork();
    ASSERT_NE(child_pid, -1);

    if (child_pid == 0) {
        ::close(child_to_parent[0]);
        ::close(parent_to_child[1]);

        Pager child_pager;
        send_pager_result(child_to_parent[1], child_pager.open(db_path.string()));
        PagerGetResult child_get_result = child_pager.get(1);
        send_pager_result(child_to_parent[1], child_get_result.status);
        wait_signal(parent_to_child[0]);
        child_pager.unref_page(1);

        ::close(child_to_parent[1]);
        ::close(parent_to_child[0]);
        _exit(0);
    }

    ::close(child_to_parent[1]);
    ::close(parent_to_child[0]);

    ASSERT_EQ(recv_pager_result(child_to_parent[0]), PagerResult::Success);
    ASSERT_EQ(recv_pager_result(child_to_parent[0]), PagerResult::Success);

    Pager writer_pager;
    ASSERT_EQ(writer_pager.open(db_path.string()), PagerResult::Success);
    PagerGetResult writer_get_result = writer_pager.get(1);
    ASSERT_EQ(writer_get_result.status, PagerResult::Success);
    ASSERT_EQ(writer_pager.begin_write(1), PagerResult::Success);

    auto updated_page_bytes = make_filled_page('W');
    std::memcpy(writer_get_result.data, updated_page_bytes.data(), PAGE_SIZE);

    ASSERT_EQ(writer_pager.commit_phase_one(), PagerResult::Busy);

    send_signal(parent_to_child[1]);
    ::close(child_to_parent[0]);
    ::close(parent_to_child[1]);
    expect_child_exit_ok(child_pid);

    ASSERT_EQ(writer_pager.commit_phase_one(), PagerResult::Success);
    ASSERT_EQ(writer_pager.commit_phase_two(), PagerResult::Success);
    EXPECT_EQ(read_db_page(1), updated_page_bytes);
}

TEST_F(PagerIntegrationTest, GetRecoversHotJournalWrittenByAnotherProcess) {
    auto original_page_bytes = make_filled_page('X');
    auto updated_page_bytes = make_filled_page('Y');

    {
        Pager setup_pager;
        ASSERT_EQ(setup_pager.open(db_path.string()), PagerResult::Success);

        PagerAllocateResult allocate_result = setup_pager.allocate_page();
        ASSERT_EQ(allocate_result.status, PagerResult::Success);
        ASSERT_EQ(allocate_result.page_num, 1);
        std::memcpy(allocate_result.data, original_page_bytes.data(), PAGE_SIZE);

        ASSERT_EQ(setup_pager.commit_phase_one(), PagerResult::Success);
        ASSERT_EQ(setup_pager.commit_phase_two(), PagerResult::Success);
    }

    Pager recovery_pager;
    ASSERT_EQ(recovery_pager.open(db_path.string()), PagerResult::Success);

    int child_to_parent[2];
    ASSERT_EQ(::pipe(child_to_parent), 0);

    pid_t child_pid = ::fork();
    ASSERT_NE(child_pid, -1);

    if (child_pid == 0) {
        ::close(child_to_parent[0]);

        Pager writer_pager;
        send_pager_result(child_to_parent[1], writer_pager.open(db_path.string()));
        PagerGetResult writer_get_result = writer_pager.get(1);
        send_pager_result(child_to_parent[1], writer_get_result.status);
        send_pager_result(child_to_parent[1], writer_pager.begin_write(1));
        std::memcpy(writer_get_result.data, updated_page_bytes.data(), PAGE_SIZE);
        send_pager_result(child_to_parent[1], writer_pager.commit_phase_one());

        ::close(child_to_parent[1]);
        _exit(0);
    }

    ::close(child_to_parent[1]);

    ASSERT_EQ(recv_pager_result(child_to_parent[0]), PagerResult::Success);
    ASSERT_EQ(recv_pager_result(child_to_parent[0]), PagerResult::Success);
    ASSERT_EQ(recv_pager_result(child_to_parent[0]), PagerResult::Success);
    ASSERT_EQ(recv_pager_result(child_to_parent[0]), PagerResult::Success);
    ::close(child_to_parent[0]);
    expect_child_exit_ok(child_pid);

    PagerGetResult recovery_get_result = recovery_pager.get(1);
    ASSERT_EQ(recovery_get_result.status, PagerResult::Success);
    for (int i = 0; i < PAGE_SIZE; i++) {
        EXPECT_EQ(recovery_get_result.data[i], 'X');
    }
    EXPECT_EQ(read_db_page(1), original_page_bytes);
    EXPECT_TRUE(journal_exists());
    EXPECT_EQ(journal_size(), 0u);
}

TEST_F(PagerIntegrationTest, GetRefreshesStaleHeaderAfterOtherProcessAppendCommit) {
    Pager stale_reader_pager;
    ASSERT_EQ(stale_reader_pager.open(db_path.string()), PagerResult::Success);

    int child_to_parent[2];
    ASSERT_EQ(::pipe(child_to_parent), 0);

    pid_t child_pid = ::fork();
    ASSERT_NE(child_pid, -1);

    if (child_pid == 0) {
        ::close(child_to_parent[0]);

        Pager writer_pager;
        send_pager_result(child_to_parent[1], writer_pager.open(db_path.string()));
        PagerAllocateResult allocate_result = writer_pager.allocate_page();
        send_pager_result(child_to_parent[1], allocate_result.status);
        auto page_bytes = make_filled_page('Q');
        std::memcpy(allocate_result.data, page_bytes.data(), PAGE_SIZE);
        send_pager_result(child_to_parent[1], writer_pager.commit_phase_one());
        send_pager_result(child_to_parent[1], writer_pager.commit_phase_two());

        ::close(child_to_parent[1]);
        _exit(0);
    }

    ::close(child_to_parent[1]);
    ASSERT_EQ(recv_pager_result(child_to_parent[0]), PagerResult::Success);
    ASSERT_EQ(recv_pager_result(child_to_parent[0]), PagerResult::Success);
    ASSERT_EQ(recv_pager_result(child_to_parent[0]), PagerResult::Success);
    ASSERT_EQ(recv_pager_result(child_to_parent[0]), PagerResult::Success);
    ::close(child_to_parent[0]);
    expect_child_exit_ok(child_pid);

    PagerGetResult get_result = stale_reader_pager.get(1);
    ASSERT_EQ(get_result.status, PagerResult::Success);
    for (int i = 0; i < PAGE_SIZE; i++) {
        EXPECT_EQ(get_result.data[i], 'Q');
    }
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

TEST_F(PagerIntegrationTest, BeginWriteOnExistingPageRollbackRestoresOriginalBytes) {
    Pager writer_pager;
    ASSERT_EQ(writer_pager.open(db_path.string()), PagerResult::Success);
    PagerAllocateResult allocate_result = writer_pager.allocate_page();
    ASSERT_EQ(allocate_result.status, PagerResult::Success);
    EXPECT_EQ(allocate_result.page_num, 1);
    auto page_bytes = make_filled_page('A');
    std::memcpy(allocate_result.data, page_bytes.data(), PAGE_SIZE);

    ASSERT_EQ(writer_pager.commit_phase_one(), PagerResult::Success);
    ASSERT_EQ(writer_pager.commit_phase_two(), PagerResult::Success);

    PagerGetResult get_result = writer_pager.get(1);
    ASSERT_EQ(get_result.status, PagerResult::Success);
    ASSERT_EQ(writer_pager.begin_write(1), PagerResult::Success);
    for (int i = 0; i < PAGE_SIZE; i++) {
        get_result.data[i] = 'B';
    }

    ASSERT_EQ(writer_pager.rollback_transaction(), PagerResult::Success);

    // The overwrite never made it to a durable journal, so the original page bytes should be restored
    for (int i = 0; i < PAGE_SIZE; i++) {
        EXPECT_EQ(get_result.data[i], 'A');
    }

    EXPECT_EQ(read_db_page(1), page_bytes);
    EXPECT_TRUE(!journal_exists() || std::filesystem::is_empty(journal_path));

    DBHeader header = read_db_header();
    EXPECT_EQ(header.db_page_count, 2u);
    EXPECT_EQ(header.file_change_counter, 1u);
    EXPECT_EQ(header.freelist_head_page_num, 0u);
    EXPECT_EQ(header.freelist_page_count, 0u);

    Pager recovery_pager;
    ASSERT_EQ(recovery_pager.open(db_path.string()), PagerResult::Success);

    PagerGetResult recovery_get_result = recovery_pager.get(1);
    ASSERT_EQ(recovery_get_result.status, PagerResult::Success);
    for (int i = 0; i < PAGE_SIZE; i++) {
        EXPECT_EQ(recovery_get_result.data[i], 'A');
    }

}

TEST_F(PagerIntegrationTest, OpenRecoversHotJournalAfterExistingPageOverwrite) {
    auto original_page_bytes = make_filled_page('A');
    auto updated_page_bytes = make_filled_page('B');

    {
        Pager writer_pager;
        ASSERT_EQ(writer_pager.open(db_path.string()), PagerResult::Success);

        PagerAllocateResult allocate_result = writer_pager.allocate_page();
        ASSERT_EQ(allocate_result.status, PagerResult::Success);
        ASSERT_EQ(allocate_result.page_num, 1);
        std::memcpy(allocate_result.data, original_page_bytes.data(), PAGE_SIZE);

        ASSERT_EQ(writer_pager.commit_phase_one(), PagerResult::Success);
        ASSERT_EQ(writer_pager.commit_phase_two(), PagerResult::Success);

        PagerGetResult get_result = writer_pager.get(1);
        ASSERT_EQ(get_result.status, PagerResult::Success);
        ASSERT_EQ(writer_pager.begin_write(1), PagerResult::Success);
        std::memcpy(get_result.data, updated_page_bytes.data(), PAGE_SIZE);

        ASSERT_EQ(writer_pager.commit_phase_one(), PagerResult::Success);
        EXPECT_TRUE(journal_exists());
        EXPECT_GT(journal_size(), 0u);
        EXPECT_EQ(read_db_page(1), updated_page_bytes);
    }

    EXPECT_TRUE(journal_exists());
    EXPECT_GT(journal_size(), 0u);
    EXPECT_EQ(read_db_page(1), updated_page_bytes);

    Pager recovery_pager;
    ASSERT_EQ(recovery_pager.open(db_path.string()), PagerResult::Success);

    EXPECT_TRUE(journal_exists());
    EXPECT_EQ(journal_size(), 0u);
    EXPECT_EQ(read_db_page(1), original_page_bytes);

    DBHeader header = read_db_header();
    EXPECT_EQ(header.db_page_count, 2u);
    EXPECT_EQ(header.file_change_counter, 1u);
    EXPECT_EQ(header.freelist_head_page_num, 0u);
    EXPECT_EQ(header.freelist_page_count, 0u);

    PagerGetResult recovery_get_result = recovery_pager.get(1);
    ASSERT_EQ(recovery_get_result.status, PagerResult::Success);
    for (int i = 0; i < PAGE_SIZE; i++) {
        EXPECT_EQ(recovery_get_result.data[i], 'A');
    }
}

TEST_F(PagerIntegrationTest, FreePageRollbackRestoresFreelistState) {
    Pager pager;
    ASSERT_EQ(pager.open(db_path.string()), PagerResult::Success);

    PagerAllocateResult allocate_result = pager.allocate_page();
    ASSERT_EQ(allocate_result.status, PagerResult::Success);
    ASSERT_EQ(allocate_result.page_num, 1);

    auto page_bytes = make_filled_page('C');
    std::memcpy(allocate_result.data, page_bytes.data(), PAGE_SIZE);

    ASSERT_EQ(pager.commit_phase_one(), PagerResult::Success);
    ASSERT_EQ(pager.commit_phase_two(), PagerResult::Success);
    ASSERT_EQ(pager.unref_page(1), PagerResult::Success);

    DBHeader header = read_db_header();
    EXPECT_EQ(header.db_page_count, 2u);
    EXPECT_EQ(header.file_change_counter, 1u);
    EXPECT_EQ(header.freelist_head_page_num, 0u);
    EXPECT_EQ(header.freelist_page_count, 0u);

    ASSERT_EQ(pager.free_page(1), PagerResult::Success);
    ASSERT_EQ(pager.rollback_transaction(), PagerResult::Success);

    // The rollback should restore the header freelist metadata and leave the page as a live payload page.
    header = read_db_header();
    EXPECT_EQ(header.db_page_count, 2u);
    EXPECT_EQ(header.file_change_counter, 1u);
    EXPECT_EQ(header.freelist_head_page_num, 0u);
    EXPECT_EQ(header.freelist_page_count, 0u);
    EXPECT_TRUE(!journal_exists() || std::filesystem::is_empty(journal_path));

    PagerGetResult get_result = pager.get(1);
    ASSERT_EQ(get_result.status, PagerResult::Success);
    for (int i = 0; i < PAGE_SIZE; i++) {
        EXPECT_EQ(get_result.data[i], 'C');
    }
    EXPECT_EQ(read_db_page(1), page_bytes);

    Pager recovery_pager;
    ASSERT_EQ(recovery_pager.open(db_path.string()), PagerResult::Success);

    PagerGetResult recovery_get_result = recovery_pager.get(1);
    ASSERT_EQ(recovery_get_result.status, PagerResult::Success);
    for (int i = 0; i < PAGE_SIZE; i++) {
        EXPECT_EQ(recovery_get_result.data[i], 'C');
    }
}

TEST_F(PagerIntegrationTest, OpenRecoversHotJournalAfterFreelistPageReuse) {
    auto original_page_bytes = make_filled_page('D');
    auto reused_page_bytes = make_filled_page('E');

    {
        Pager pager;
        ASSERT_EQ(pager.open(db_path.string()), PagerResult::Success);

        PagerAllocateResult allocate_result = pager.allocate_page();
        ASSERT_EQ(allocate_result.status, PagerResult::Success);
        ASSERT_EQ(allocate_result.page_num, 1);
        std::memcpy(allocate_result.data, original_page_bytes.data(), PAGE_SIZE);

        ASSERT_EQ(pager.commit_phase_one(), PagerResult::Success);
        ASSERT_EQ(pager.commit_phase_two(), PagerResult::Success);
        ASSERT_EQ(pager.unref_page(1), PagerResult::Success);

        ASSERT_EQ(pager.free_page(1), PagerResult::Success);
        ASSERT_EQ(pager.commit_phase_one(), PagerResult::Success);
        ASSERT_EQ(pager.commit_phase_two(), PagerResult::Success);
    }

    DBHeader header = read_db_header();
    EXPECT_EQ(header.db_page_count, 2u);
    EXPECT_EQ(header.freelist_head_page_num, 1u);
    EXPECT_EQ(header.freelist_page_count, 1u);

    {
        Pager writer_pager;
        ASSERT_EQ(writer_pager.open(db_path.string()), PagerResult::Success);

        PagerAllocateResult reuse_result = writer_pager.allocate_page();
        ASSERT_EQ(reuse_result.status, PagerResult::Success);
        ASSERT_EQ(reuse_result.page_num, 1);
        std::memcpy(reuse_result.data, reused_page_bytes.data(), PAGE_SIZE);

        ASSERT_EQ(writer_pager.commit_phase_one(), PagerResult::Success);
        EXPECT_TRUE(journal_exists());
        EXPECT_GT(journal_size(), 0u);

        header = read_db_header();
        EXPECT_EQ(header.freelist_head_page_num, 0u);
        EXPECT_EQ(header.freelist_page_count, 0u);
        EXPECT_EQ(read_db_page(1), reused_page_bytes);
    }

    Pager recovery_pager;
    ASSERT_EQ(recovery_pager.open(db_path.string()), PagerResult::Success);

    EXPECT_TRUE(journal_exists());
    EXPECT_EQ(journal_size(), 0u);

    header = read_db_header();
    EXPECT_EQ(header.db_page_count, 2u);
    EXPECT_EQ(header.freelist_head_page_num, 1u);
    EXPECT_EQ(header.freelist_page_count, 1u);

    PagerAllocateResult recovered_reuse_result = recovery_pager.allocate_page();
    ASSERT_EQ(recovered_reuse_result.status, PagerResult::Success);
    EXPECT_EQ(recovered_reuse_result.page_num, 1);

    std::array<char, PAGE_SIZE> zero_page{};
    EXPECT_EQ(std::memcmp(recovered_reuse_result.data, zero_page.data(), PAGE_SIZE), 0);
}

TEST_F(PagerIntegrationTest, OpenRecoversHotJournalWithMultipleJournalSections) {
    auto original_page_bytes = make_filled_page('A');
    auto first_update_bytes = make_filled_page('B');
    auto second_update_bytes = make_filled_page('C');

    {
        Pager writer_pager;
        ASSERT_EQ(writer_pager.open(db_path.string()), PagerResult::Success);

        PagerAllocateResult allocate_result = writer_pager.allocate_page();
        ASSERT_EQ(allocate_result.status, PagerResult::Success);
        ASSERT_EQ(allocate_result.page_num, 1);
        std::memcpy(allocate_result.data, original_page_bytes.data(), PAGE_SIZE);

        ASSERT_EQ(writer_pager.commit_phase_one(), PagerResult::Success);
        ASSERT_EQ(writer_pager.commit_phase_two(), PagerResult::Success);

        PagerGetResult get_result = writer_pager.get(1);
        ASSERT_EQ(get_result.status, PagerResult::Success);

        ASSERT_EQ(writer_pager.begin_write(1), PagerResult::Success);
        std::memcpy(get_result.data, first_update_bytes.data(), PAGE_SIZE);
        ASSERT_EQ(writer_pager.commit_phase_one(), PagerResult::Success);
        EXPECT_TRUE(journal_exists());
        EXPECT_GT(journal_size(), 0u);
        EXPECT_EQ(read_db_page(1), first_update_bytes);

        ASSERT_EQ(writer_pager.begin_write(1), PagerResult::Success);
        std::memcpy(get_result.data, second_update_bytes.data(), PAGE_SIZE);
        ASSERT_EQ(writer_pager.commit_phase_one(), PagerResult::Success);
        EXPECT_EQ(read_db_page(1), second_update_bytes);
    }

    EXPECT_TRUE(journal_exists());
    EXPECT_GT(journal_size(), static_cast<std::uintmax_t>(PAGE_SIZE + JOURNAL_PAGE_RECORD * 2));
    EXPECT_EQ(read_db_page(1), second_update_bytes);

    Pager recovery_pager;
    ASSERT_EQ(recovery_pager.open(db_path.string()), PagerResult::Success);

    EXPECT_TRUE(journal_exists());
    EXPECT_EQ(journal_size(), 0u);
    EXPECT_EQ(read_db_page(1), original_page_bytes);

    DBHeader header = read_db_header();
    EXPECT_EQ(header.db_page_count, 2u);
    EXPECT_EQ(header.file_change_counter, 1u);
    EXPECT_EQ(header.freelist_head_page_num, 0u);
    EXPECT_EQ(header.freelist_page_count, 0u);

    PagerGetResult recovery_get_result = recovery_pager.get(1);
    ASSERT_EQ(recovery_get_result.status, PagerResult::Success);
    for (int i = 0; i < PAGE_SIZE; i++) {
        EXPECT_EQ(recovery_get_result.data[i], 'A');
    }
}

TEST_F(PagerIntegrationTest, OpenRecoversHotJournalAfterFreePage) {
    auto original_page_bytes = make_filled_page('F');

    {
        Pager writer_pager;
        ASSERT_EQ(writer_pager.open(db_path.string()), PagerResult::Success);

        PagerAllocateResult allocate_result = writer_pager.allocate_page();
        ASSERT_EQ(allocate_result.status, PagerResult::Success);
        ASSERT_EQ(allocate_result.page_num, 1);
        std::memcpy(allocate_result.data, original_page_bytes.data(), PAGE_SIZE);

        ASSERT_EQ(writer_pager.commit_phase_one(), PagerResult::Success);
        ASSERT_EQ(writer_pager.commit_phase_two(), PagerResult::Success);
        ASSERT_EQ(writer_pager.unref_page(1), PagerResult::Success);

        ASSERT_EQ(writer_pager.free_page(1), PagerResult::Success);
        ASSERT_EQ(writer_pager.commit_phase_one(), PagerResult::Success);

        EXPECT_TRUE(journal_exists());
        EXPECT_GT(journal_size(), 0u);

        DBHeader header = read_db_header();
        EXPECT_EQ(header.db_page_count, 2u);
        EXPECT_EQ(header.freelist_head_page_num, 1u);
        EXPECT_EQ(header.freelist_page_count, 1u);
    }

    EXPECT_TRUE(journal_exists());
    EXPECT_GT(journal_size(), 0u);

    Pager recovery_pager;
    ASSERT_EQ(recovery_pager.open(db_path.string()), PagerResult::Success);

    EXPECT_TRUE(journal_exists());
    EXPECT_EQ(journal_size(), 0u);

    DBHeader header = read_db_header();
    EXPECT_EQ(header.db_page_count, 2u);
    EXPECT_EQ(header.file_change_counter, 1u);
    EXPECT_EQ(header.freelist_head_page_num, 0u);
    EXPECT_EQ(header.freelist_page_count, 0u);

    EXPECT_EQ(read_db_page(1), original_page_bytes);

    PagerGetResult recovery_get_result = recovery_pager.get(1);
    ASSERT_EQ(recovery_get_result.status, PagerResult::Success);
    for (int i = 0; i < PAGE_SIZE; i++) {
        EXPECT_EQ(recovery_get_result.data[i], 'F');
    }
}

TEST_F(PagerIntegrationTest, FreePageWithExistingFreelistRollbackRestoresLinks) {
    auto first_page_bytes = make_filled_page('G');
    auto second_page_bytes = make_filled_page('H');

    Pager pager;
    ASSERT_EQ(pager.open(db_path.string()), PagerResult::Success);

    PagerAllocateResult first_allocate_result = pager.allocate_page();
    ASSERT_EQ(first_allocate_result.status, PagerResult::Success);
    ASSERT_EQ(first_allocate_result.page_num, 1);
    std::memcpy(first_allocate_result.data, first_page_bytes.data(), PAGE_SIZE);

    PagerAllocateResult second_allocate_result = pager.allocate_page();
    ASSERT_EQ(second_allocate_result.status, PagerResult::Success);
    ASSERT_EQ(second_allocate_result.page_num, 2);
    std::memcpy(second_allocate_result.data, second_page_bytes.data(), PAGE_SIZE);

    ASSERT_EQ(pager.commit_phase_one(), PagerResult::Success);
    ASSERT_EQ(pager.commit_phase_two(), PagerResult::Success);
    ASSERT_EQ(pager.unref_page(1), PagerResult::Success);
    ASSERT_EQ(pager.unref_page(2), PagerResult::Success);

    ASSERT_EQ(pager.free_page(1), PagerResult::Success);
    ASSERT_EQ(pager.commit_phase_one(), PagerResult::Success);
    ASSERT_EQ(pager.commit_phase_two(), PagerResult::Success);

    DBHeader header = read_db_header();
    EXPECT_EQ(header.db_page_count, 3u);
    EXPECT_EQ(header.freelist_head_page_num, 1u);
    EXPECT_EQ(header.freelist_page_count, 1u);

    ASSERT_EQ(pager.free_page(2), PagerResult::Success);
    ASSERT_EQ(pager.rollback_transaction(), PagerResult::Success);

    header = read_db_header();
    EXPECT_EQ(header.db_page_count, 3u);
    EXPECT_EQ(header.file_change_counter, 2u);
    EXPECT_EQ(header.freelist_head_page_num, 1u);
    EXPECT_EQ(header.freelist_page_count, 1u);
    EXPECT_TRUE(!journal_exists() || std::filesystem::is_empty(journal_path));

    PagerGetResult second_get_result = pager.get(2);
    ASSERT_EQ(second_get_result.status, PagerResult::Success);
    for (int i = 0; i < PAGE_SIZE; i++) {
        EXPECT_EQ(second_get_result.data[i], 'H');
    }
    EXPECT_EQ(read_db_page(2), second_page_bytes);

    PagerAllocateResult reuse_result = pager.allocate_page();
    ASSERT_EQ(reuse_result.status, PagerResult::Success);
    EXPECT_EQ(reuse_result.page_num, 1);

    std::array<char, PAGE_SIZE> zero_page{};
    EXPECT_EQ(std::memcmp(reuse_result.data, zero_page.data(), PAGE_SIZE), 0);
}

TEST_F(PagerIntegrationTest, OpenRecoversHotJournalAfterFreePageWithExistingFreelist) {
    auto first_page_bytes = make_filled_page('I');
    auto second_page_bytes = make_filled_page('J');

    {
        Pager pager;
        ASSERT_EQ(pager.open(db_path.string()), PagerResult::Success);

        PagerAllocateResult first_allocate_result = pager.allocate_page();
        ASSERT_EQ(first_allocate_result.status, PagerResult::Success);
        ASSERT_EQ(first_allocate_result.page_num, 1);
        std::memcpy(first_allocate_result.data, first_page_bytes.data(), PAGE_SIZE);

        PagerAllocateResult second_allocate_result = pager.allocate_page();
        ASSERT_EQ(second_allocate_result.status, PagerResult::Success);
        ASSERT_EQ(second_allocate_result.page_num, 2);
        std::memcpy(second_allocate_result.data, second_page_bytes.data(), PAGE_SIZE);

        ASSERT_EQ(pager.commit_phase_one(), PagerResult::Success);
        ASSERT_EQ(pager.commit_phase_two(), PagerResult::Success);
        ASSERT_EQ(pager.unref_page(1), PagerResult::Success);
        ASSERT_EQ(pager.unref_page(2), PagerResult::Success);

        ASSERT_EQ(pager.free_page(1), PagerResult::Success);
        ASSERT_EQ(pager.commit_phase_one(), PagerResult::Success);
        ASSERT_EQ(pager.commit_phase_two(), PagerResult::Success);

        ASSERT_EQ(pager.free_page(2), PagerResult::Success);
        ASSERT_EQ(pager.commit_phase_one(), PagerResult::Success);

        EXPECT_TRUE(journal_exists());
        EXPECT_GT(journal_size(), 0u);

        DBHeader header = read_db_header();
        EXPECT_EQ(header.db_page_count, 3u);
        EXPECT_EQ(header.freelist_head_page_num, 2u);
        EXPECT_EQ(header.freelist_page_count, 2u);
    }

    EXPECT_TRUE(journal_exists());
    EXPECT_GT(journal_size(), 0u);

    Pager recovery_pager;
    ASSERT_EQ(recovery_pager.open(db_path.string()), PagerResult::Success);

    EXPECT_TRUE(journal_exists());
    EXPECT_EQ(journal_size(), 0u);

    DBHeader header = read_db_header();
    EXPECT_EQ(header.db_page_count, 3u);
    EXPECT_EQ(header.file_change_counter, 2u);
    EXPECT_EQ(header.freelist_head_page_num, 1u);
    EXPECT_EQ(header.freelist_page_count, 1u);

    EXPECT_EQ(read_db_page(2), second_page_bytes);

    PagerGetResult second_get_result = recovery_pager.get(2);
    ASSERT_EQ(second_get_result.status, PagerResult::Success);
    for (int i = 0; i < PAGE_SIZE; i++) {
        EXPECT_EQ(second_get_result.data[i], 'J');
    }

    PagerAllocateResult reuse_result = recovery_pager.allocate_page();
    ASSERT_EQ(reuse_result.status, PagerResult::Success);
    EXPECT_EQ(reuse_result.page_num, 1);

    std::array<char, PAGE_SIZE> zero_page{};
    EXPECT_EQ(std::memcmp(reuse_result.data, zero_page.data(), PAGE_SIZE), 0);
}

TEST_F(PagerIntegrationTest, OpenRecoversHotJournalAfterMultiPageOverwrite) {
    auto first_original_page_bytes = make_filled_page('K');
    auto second_original_page_bytes = make_filled_page('L');
    auto first_updated_page_bytes = make_filled_page('M');
    auto second_updated_page_bytes = make_filled_page('N');

    {
        Pager writer_pager;
        ASSERT_EQ(writer_pager.open(db_path.string()), PagerResult::Success);

        PagerAllocateResult first_allocate_result = writer_pager.allocate_page();
        ASSERT_EQ(first_allocate_result.status, PagerResult::Success);
        ASSERT_EQ(first_allocate_result.page_num, 1);
        std::memcpy(first_allocate_result.data, first_original_page_bytes.data(), PAGE_SIZE);

        PagerAllocateResult second_allocate_result = writer_pager.allocate_page();
        ASSERT_EQ(second_allocate_result.status, PagerResult::Success);
        ASSERT_EQ(second_allocate_result.page_num, 2);
        std::memcpy(second_allocate_result.data, second_original_page_bytes.data(), PAGE_SIZE);

        ASSERT_EQ(writer_pager.commit_phase_one(), PagerResult::Success);
        ASSERT_EQ(writer_pager.commit_phase_two(), PagerResult::Success);

        PagerGetResult first_get_result = writer_pager.get(1);
        ASSERT_EQ(first_get_result.status, PagerResult::Success);
        ASSERT_EQ(writer_pager.begin_write(1), PagerResult::Success);
        std::memcpy(first_get_result.data, first_updated_page_bytes.data(), PAGE_SIZE);

        PagerGetResult second_get_result = writer_pager.get(2);
        ASSERT_EQ(second_get_result.status, PagerResult::Success);
        ASSERT_EQ(writer_pager.begin_write(2), PagerResult::Success);
        std::memcpy(second_get_result.data, second_updated_page_bytes.data(), PAGE_SIZE);

        ASSERT_EQ(writer_pager.commit_phase_one(), PagerResult::Success);
        EXPECT_TRUE(journal_exists());
        EXPECT_GT(journal_size(), 0u);
        EXPECT_EQ(read_db_page(1), first_updated_page_bytes);
        EXPECT_EQ(read_db_page(2), second_updated_page_bytes);
    }

    EXPECT_TRUE(journal_exists());
    EXPECT_GT(journal_size(), 0u);

    Pager recovery_pager;
    ASSERT_EQ(recovery_pager.open(db_path.string()), PagerResult::Success);

    EXPECT_TRUE(journal_exists());
    EXPECT_EQ(journal_size(), 0u);
    EXPECT_EQ(read_db_page(1), first_original_page_bytes);
    EXPECT_EQ(read_db_page(2), second_original_page_bytes);

    DBHeader header = read_db_header();
    EXPECT_EQ(header.db_page_count, 3u);
    EXPECT_EQ(header.file_change_counter, 1u);
    EXPECT_EQ(header.freelist_head_page_num, 0u);
    EXPECT_EQ(header.freelist_page_count, 0u);

    PagerGetResult first_recovery_get_result = recovery_pager.get(1);
    ASSERT_EQ(first_recovery_get_result.status, PagerResult::Success);
    for (int i = 0; i < PAGE_SIZE; i++) {
        EXPECT_EQ(first_recovery_get_result.data[i], 'K');
    }

    PagerGetResult second_recovery_get_result = recovery_pager.get(2);
    ASSERT_EQ(second_recovery_get_result.status, PagerResult::Success);
    for (int i = 0; i < PAGE_SIZE; i++) {
        EXPECT_EQ(second_recovery_get_result.data[i], 'L');
    }
}

} // namespace

#include <gtest/gtest.h>

#include <DiskIO.h>
#include <LockMgr.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

class LockMgrIntegrationTest : public ::testing::Test {
    protected:
        void SetUp() override {
            auto unique_suffix = std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()
            );
            temp_dir = std::filesystem::temp_directory_path() / ("stoneleafdb_lockmgr_itest_" + unique_suffix);
            db_path = temp_dir / "test.db";
            std::filesystem::create_directories(temp_dir);

            int fd = disk::open_file(db_path.string(), O_RDWR | O_CREAT | O_TRUNC, 0644);
            disk::close_file(fd);
        }

        void TearDown() override {
            std::error_code ec;
            std::filesystem::remove_all(temp_dir, ec);
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

        void send_status(int fd, LockMgrStatus status) {
            auto raw = static_cast<std::uint8_t>(status);
            write_exact_fd(fd, &raw, sizeof(raw));
        }

        LockMgrStatus recv_status(int fd) {
            std::uint8_t raw = 0;
            read_exact_fd(fd, &raw, sizeof(raw));
            return static_cast<LockMgrStatus>(raw);
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
};

TEST_F(LockMgrIntegrationTest, MultipleReadersCanHoldSharedLock) {
    int child_to_parent[2];
    int parent_to_child[2];
    ASSERT_EQ(::pipe(child_to_parent), 0);
    ASSERT_EQ(::pipe(parent_to_child), 0);

    pid_t child_pid = ::fork();
    ASSERT_NE(child_pid, -1);

    if (child_pid == 0) {
        ::close(child_to_parent[0]);
        ::close(parent_to_child[1]);

        int fd = disk::open_file(db_path.string(), O_RDWR);
        LockMgr lock_mgr;
        send_status(child_to_parent[1], lock_mgr.lock(fd, Lock::SHARED));
        wait_signal(parent_to_child[0]);
        lock_mgr.release_all_locks(fd);
        disk::close_file(fd);
        ::close(child_to_parent[1]);
        ::close(parent_to_child[0]);
        _exit(0);
    }

    ::close(child_to_parent[1]);
    ::close(parent_to_child[0]);

    ASSERT_EQ(recv_status(child_to_parent[0]), LockMgrStatus::Success);

    int fd = disk::open_file(db_path.string(), O_RDWR);
    LockMgr lock_mgr;
    EXPECT_EQ(lock_mgr.lock(fd, Lock::SHARED), LockMgrStatus::Success);
    EXPECT_EQ(lock_mgr.get_curr_lock(), Lock::SHARED);

    send_signal(parent_to_child[1]);
    EXPECT_EQ(lock_mgr.release_all_locks(fd), LockMgrStatus::Success);
    EXPECT_NO_THROW(disk::close_file(fd));
    ::close(child_to_parent[0]);
    ::close(parent_to_child[1]);
    expect_child_exit_ok(child_pid);
}

TEST_F(LockMgrIntegrationTest, ReservedPreventsSecondWriter) {
    int child_to_parent[2];
    int parent_to_child[2];
    ASSERT_EQ(::pipe(child_to_parent), 0);
    ASSERT_EQ(::pipe(parent_to_child), 0);

    pid_t child_pid = ::fork();
    ASSERT_NE(child_pid, -1);

    if (child_pid == 0) {
        ::close(child_to_parent[0]);
        ::close(parent_to_child[1]);

        int fd = disk::open_file(db_path.string(), O_RDWR);
        LockMgr lock_mgr;
        send_status(child_to_parent[1], lock_mgr.lock(fd, Lock::RESERVED));
        wait_signal(parent_to_child[0]);
        lock_mgr.release_all_locks(fd);
        disk::close_file(fd);
        ::close(child_to_parent[1]);
        ::close(parent_to_child[0]);
        _exit(0);
    }

    ::close(child_to_parent[1]);
    ::close(parent_to_child[0]);

    ASSERT_EQ(recv_status(child_to_parent[0]), LockMgrStatus::Success);

    int fd = disk::open_file(db_path.string(), O_RDWR);
    LockMgr lock_mgr;
    EXPECT_EQ(lock_mgr.lock(fd, Lock::RESERVED), LockMgrStatus::Busy);
    EXPECT_EQ(lock_mgr.get_curr_lock(), Lock::NOLOCK);

    send_signal(parent_to_child[1]);
    EXPECT_NO_THROW(disk::close_file(fd));
    ::close(child_to_parent[0]);
    ::close(parent_to_child[1]);
    expect_child_exit_ok(child_pid);
}

TEST_F(LockMgrIntegrationTest, ExclusiveFromNoLockFailsWhileReaderExists) {
    int child_to_parent[2];
    int parent_to_child[2];
    ASSERT_EQ(::pipe(child_to_parent), 0);
    ASSERT_EQ(::pipe(parent_to_child), 0);

    pid_t child_pid = ::fork();
    ASSERT_NE(child_pid, -1);

    if (child_pid == 0) {
        ::close(child_to_parent[0]);
        ::close(parent_to_child[1]);

        int fd = disk::open_file(db_path.string(), O_RDWR);
        LockMgr lock_mgr;
        send_status(child_to_parent[1], lock_mgr.lock(fd, Lock::SHARED));
        wait_signal(parent_to_child[0]);
        lock_mgr.release_all_locks(fd);
        disk::close_file(fd);
        ::close(child_to_parent[1]);
        ::close(parent_to_child[0]);
        _exit(0);
    }

    ::close(child_to_parent[1]);
    ::close(parent_to_child[0]);

    ASSERT_EQ(recv_status(child_to_parent[0]), LockMgrStatus::Success);

    int fd = disk::open_file(db_path.string(), O_RDWR);
    LockMgr lock_mgr;
    EXPECT_EQ(lock_mgr.lock(fd, Lock::EXCLUSIVE), LockMgrStatus::Busy);
    EXPECT_EQ(lock_mgr.get_curr_lock(), Lock::NOLOCK);

    send_signal(parent_to_child[1]);
    EXPECT_NO_THROW(disk::close_file(fd));
    ::close(child_to_parent[0]);
    ::close(parent_to_child[1]);
    expect_child_exit_ok(child_pid);
}

TEST_F(LockMgrIntegrationTest, ReservedWriterCanRetryExclusiveAfterReaderReleases) {
    int child_to_parent[2];
    int parent_to_child[2];
    ASSERT_EQ(::pipe(child_to_parent), 0);
    ASSERT_EQ(::pipe(parent_to_child), 0);

    pid_t child_pid = ::fork();
    ASSERT_NE(child_pid, -1);

    if (child_pid == 0) {
        ::close(child_to_parent[0]);
        ::close(parent_to_child[1]);

        int fd = disk::open_file(db_path.string(), O_RDWR);
        LockMgr lock_mgr;
        send_status(child_to_parent[1], lock_mgr.lock(fd, Lock::SHARED));
        wait_signal(parent_to_child[0]);
        lock_mgr.release_all_locks(fd);
        disk::close_file(fd);
        ::close(child_to_parent[1]);
        ::close(parent_to_child[0]);
        _exit(0);
    }

    ::close(child_to_parent[1]);
    ::close(parent_to_child[0]);

    ASSERT_EQ(recv_status(child_to_parent[0]), LockMgrStatus::Success);

    int fd = disk::open_file(db_path.string(), O_RDWR);
    LockMgr lock_mgr;
    ASSERT_EQ(lock_mgr.lock(fd, Lock::RESERVED), LockMgrStatus::Success);
    EXPECT_EQ(lock_mgr.get_curr_lock(), Lock::RESERVED);

    EXPECT_EQ(lock_mgr.lock(fd, Lock::EXCLUSIVE), LockMgrStatus::Busy);
    EXPECT_EQ(lock_mgr.get_curr_lock(), Lock::RESERVED);

    send_signal(parent_to_child[1]);
    expect_child_exit_ok(child_pid);

    EXPECT_EQ(lock_mgr.lock(fd, Lock::EXCLUSIVE), LockMgrStatus::Success);
    EXPECT_EQ(lock_mgr.get_curr_lock(), Lock::EXCLUSIVE);

    EXPECT_EQ(lock_mgr.release_all_locks(fd), LockMgrStatus::Success);
    EXPECT_NO_THROW(disk::close_file(fd));
    ::close(child_to_parent[0]);
    ::close(parent_to_child[1]);
}

}

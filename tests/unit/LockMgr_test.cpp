#include <gtest/gtest.h>

#include <DiskIO.h>
#include <LockMgr.h>

#include <chrono>
#include <filesystem>
#include <fcntl.h>
#include <string>

namespace {

class TempFile {
    public:
        TempFile() {
            auto unique_suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            path = std::filesystem::temp_directory_path() / ("stoneleafdb_lockmgr_test_" + unique_suffix + ".db");
        }

        ~TempFile() {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }

        std::filesystem::path path;
};

TEST(LockMgrTest, StartsInNoLock) {
    LockMgr lock_mgr;

    EXPECT_EQ(lock_mgr.get_curr_lock(), Lock::NOLOCK);
}

TEST(LockMgrTest, AcquiresSharedFromNoLock) {
    TempFile temp_file;
    int fd = disk::open_file(temp_file.path.string(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    LockMgr lock_mgr;

    EXPECT_EQ(lock_mgr.lock(fd, Lock::SHARED), LockMgrStatus::Success);
    EXPECT_EQ(lock_mgr.get_curr_lock(), Lock::SHARED);

    EXPECT_EQ(lock_mgr.release_all_locks(fd), LockMgrStatus::Success);
    EXPECT_EQ(lock_mgr.get_curr_lock(), Lock::NOLOCK);
    EXPECT_NO_THROW(disk::close_file(fd));
}

TEST(LockMgrTest, AcquiresReservedFromNoLock) {
    TempFile temp_file;
    int fd = disk::open_file(temp_file.path.string(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    LockMgr lock_mgr;

    EXPECT_EQ(lock_mgr.lock(fd, Lock::RESERVED), LockMgrStatus::Success);
    EXPECT_EQ(lock_mgr.get_curr_lock(), Lock::RESERVED);

    EXPECT_EQ(lock_mgr.release_all_locks(fd), LockMgrStatus::Success);
    EXPECT_NO_THROW(disk::close_file(fd));
}

TEST(LockMgrTest, AcquiresExclusiveFromNoLock) {
    TempFile temp_file;
    int fd = disk::open_file(temp_file.path.string(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    LockMgr lock_mgr;

    EXPECT_EQ(lock_mgr.lock(fd, Lock::EXCLUSIVE), LockMgrStatus::Success);
    EXPECT_EQ(lock_mgr.get_curr_lock(), Lock::EXCLUSIVE);

    EXPECT_EQ(lock_mgr.release_all_locks(fd), LockMgrStatus::Success);
    EXPECT_NO_THROW(disk::close_file(fd));
}

TEST(LockMgrTest, SharedToExclusiveReturnsBusy) {
    TempFile temp_file;
    int fd = disk::open_file(temp_file.path.string(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    LockMgr lock_mgr;

    EXPECT_EQ(lock_mgr.lock(fd, Lock::SHARED), LockMgrStatus::Success);
    EXPECT_EQ(lock_mgr.lock(fd, Lock::EXCLUSIVE), LockMgrStatus::Busy);
    EXPECT_EQ(lock_mgr.get_curr_lock(), Lock::SHARED);

    EXPECT_EQ(lock_mgr.release_all_locks(fd), LockMgrStatus::Success);
    EXPECT_NO_THROW(disk::close_file(fd));
}

TEST(LockMgrTest, UnlockReservedToSharedDropsWritePrivilege) {
    TempFile temp_file;
    int fd = disk::open_file(temp_file.path.string(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    LockMgr lock_mgr;

    EXPECT_EQ(lock_mgr.lock(fd, Lock::RESERVED), LockMgrStatus::Success);
    EXPECT_EQ(lock_mgr.unlock(fd, Lock::SHARED), LockMgrStatus::Success);
    EXPECT_EQ(lock_mgr.get_curr_lock(), Lock::SHARED);

    EXPECT_EQ(lock_mgr.release_all_locks(fd), LockMgrStatus::Success);
    EXPECT_NO_THROW(disk::close_file(fd));
}

TEST(LockMgrTest, UnlockExclusiveToSharedKeepsReadPrivilege) {
    TempFile temp_file;
    int fd = disk::open_file(temp_file.path.string(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    LockMgr lock_mgr;

    EXPECT_EQ(lock_mgr.lock(fd, Lock::EXCLUSIVE), LockMgrStatus::Success);
    EXPECT_EQ(lock_mgr.unlock(fd, Lock::SHARED), LockMgrStatus::Success);
    EXPECT_EQ(lock_mgr.get_curr_lock(), Lock::SHARED);

    EXPECT_EQ(lock_mgr.release_all_locks(fd), LockMgrStatus::Success);
    EXPECT_NO_THROW(disk::close_file(fd));
}

TEST(LockMgrTest, UnlockExclusiveToReservedKeepsWriterIntent) {
    TempFile temp_file;
    int fd = disk::open_file(temp_file.path.string(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    LockMgr lock_mgr;

    EXPECT_EQ(lock_mgr.lock(fd, Lock::EXCLUSIVE), LockMgrStatus::Success);
    EXPECT_EQ(lock_mgr.unlock(fd, Lock::RESERVED), LockMgrStatus::Success);
    EXPECT_EQ(lock_mgr.get_curr_lock(), Lock::RESERVED);

    EXPECT_EQ(lock_mgr.release_all_locks(fd), LockMgrStatus::Success);
    EXPECT_NO_THROW(disk::close_file(fd));
}

TEST(LockMgrTest, ReleaseAllLocksFromNoLockIsSuccess) {
    TempFile temp_file;
    int fd = disk::open_file(temp_file.path.string(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    LockMgr lock_mgr;

    EXPECT_EQ(lock_mgr.release_all_locks(fd), LockMgrStatus::Success);
    EXPECT_EQ(lock_mgr.get_curr_lock(), Lock::NOLOCK);

    EXPECT_NO_THROW(disk::close_file(fd));
}

}

#pragma once
#include <fstream>
#include <cstdint>

enum class Lock : std::uint8_t {
    NOLOCK = 0,
    SHARED,
    RESERVED,
    EXCLUSIVE
};

enum class PrimitiveLockType : std::uint8_t {
    READ = 0,
    WRITE,
};

enum class LockMgrStatus : std::uint8_t {
    Success = 0,
    Busy,
};

class LockMgr {
    public:
        LockMgr() = default;
        ~LockMgr() = default;
        LockMgrStatus lock(int fd, Lock target_state);
        LockMgrStatus unlock(int df, Lock target_state);
        LockMgrStatus release_all_locks(int fd);
        Lock get_curr_lock() const;
    private:
        enum class LockState : std::uint8_t {
            NOLOCK = 0,
            SHARED,
            RESERVED,
            PENDING,
            EXCLUSIVE
        };
        static const int MAX_EXCLUSIVE_RETRIES = 32;
        static const int MAX_PENDING_RETRIES = 32;
        static const std::streamoff PENDING_BYTE;
        static const std::streamoff RESERVED_BYTE;
        static const std::streamoff SHARED_BYTE;

        LockState lock_state = LockState::NOLOCK;
        LockMgrStatus acquire_primitive_lock(int fd, PrimitiveLockType type, std::streamoff byte);
        LockMgrStatus release_primitive_lock(int fd, std::streamoff byte);
        LockMgrStatus acquire_shared(int fd);
        LockMgrStatus acquire_reserved(int fd);
        LockMgrStatus acquire_pending(int fd);
        LockMgrStatus acquire_exclusive_from_nolock(int fd);
        LockMgrStatus acquire_exclusive_from_reserved(int fd);
};

#include <LockMgr.h>
#include <cstdlib>
#include <cerrno>
#include <fcntl.h>

LockMgrStatus LockMgr::lock(int fd, Lock target_state) {
      switch (target_state) {
          case Lock::NOLOCK:
              return LockMgrStatus::Success;

          case Lock::SHARED:
              return acquire_shared(fd);

          case Lock::RESERVED:
              return acquire_reserved(fd);

          case Lock::EXCLUSIVE:
              if (lock_state == LockState::EXCLUSIVE) return LockMgrStatus::Success;
              if (lock_state == LockState::NOLOCK) {
                  return acquire_exclusive_from_nolock(fd);
              }
              if (lock_state == LockState::RESERVED) {
                  return acquire_exclusive_from_reserved(fd);
              }
              return LockMgrStatus::Busy; // we dont allow shared -> exclusive transition
      }

      return LockMgrStatus::Busy;

}




// Helpers
LockMgrStatus LockMgr::acquire_primitive_lock(int fd, PrimitiveLockType type, std::streamoff byte) {
    // Map our enum to the actual POSIX primitive. Don't rely on enum numeric values here.
    short lock_type = F_RDLCK;
    switch (type) {
        case PrimitiveLockType::READ:
            lock_type = F_RDLCK;
            break;
        case PrimitiveLockType::WRITE:
            lock_type = F_WRLCK;
            break;
    }

    // We only ever lock one byte in the lock range. the caller decides which byte matters.
    struct flock fl {};
    fl.l_type = lock_type;
    fl.l_len = 1;
    fl.l_whence = SEEK_SET;
    fl.l_start = byte;

    // Use the non-blocking variant. retry / backoff policy belongs one level above this helper.
    if (::fcntl(fd, F_SETLK, &fl) == -1) {
        return LockMgrStatus::Busy;
    }
    return LockMgrStatus::Success;
}

LockMgrStatus LockMgr::release_primitive_lock(int fd, std::streamoff byte) {
    // Unlocking is just the same byte-range operation, except the lock type is F_UNLCK.
    struct flock fl {};
    fl.l_type = F_UNLCK;
    fl.l_len = 1;
    fl.l_whence = SEEK_SET;
    fl.l_start = byte;

    if (::fcntl(fd, F_SETLK, &fl) == -1) {
        return LockMgrStatus::Busy;
    }
    return LockMgrStatus::Success;
}

LockMgrStatus LockMgr::acquire_shared(int fd) {
    if (lock_state == LockState::NOLOCK) {
        // Acquire a read lock on the pending byte first. If we fail, we need to backoff since that means
        // Another process is trying to acquire exclusive/has exclusive
        LockMgrStatus pending_byte_read = acquire_primitive_lock(fd, PrimitiveLockType::READ, PENDING_BYTE);
        if (pending_byte_read != LockMgrStatus::Success) return LockMgrStatus::Busy;
        
        // Now acquire read on shared byte
        LockMgrStatus shared_byte_read = acquire_primitive_lock(fd, PrimitiveLockType::READ, SHARED_BYTE);
        // Once shared is acquired, we no longer need the pending-byte gate.
        if (release_primitive_lock(fd, PENDING_BYTE) != LockMgrStatus::Success) {
            std::abort();
        }
        if (shared_byte_read != LockMgrStatus::Success) {
            return LockMgrStatus::Busy;
        }
        lock_state = LockState::SHARED;
    }
    return LockMgrStatus::Success;
}

LockMgrStatus LockMgr::acquire_reserved(int fd) {
    if (lock_state != LockState::NOLOCK && lock_state != LockState::SHARED) return LockMgrStatus::Success;
    bool prev_is_shared = (lock_state == LockState::SHARED);
    if (!prev_is_shared && acquire_shared(fd) != LockMgrStatus::Success) {
        return LockMgrStatus::Busy;
    }

    // Try to acquire write lock on reserved byte to prevent other writers from writing
    LockMgrStatus reserved_byte_write = acquire_primitive_lock(fd, PrimitiveLockType::WRITE, RESERVED_BYTE);
    if (reserved_byte_write == LockMgrStatus::Busy){
        if (!prev_is_shared) {
            // We must have acquired shared in this call. Must release it before we back out.
            if (release_primitive_lock(fd, SHARED_BYTE) != LockMgrStatus::Success) {
                std::abort();
            }
            lock_state = LockState::NOLOCK;
        }
        return LockMgrStatus::Busy;
    }
    lock_state = LockState::RESERVED;
    return LockMgrStatus::Success;
}

LockMgrStatus LockMgr::acquire_exclusive_from_nolock(int fd) {
    // Just straight up acquire write lock on the shared byte. thats it
    LockMgrStatus shared_byte_write = acquire_primitive_lock(fd, PrimitiveLockType::WRITE, SHARED_BYTE);
    if (shared_byte_write != LockMgrStatus::Success) return LockMgrStatus::Busy;
    lock_state = LockState::EXCLUSIVE;
    return LockMgrStatus::Success;
}

LockMgrStatus LockMgr::acquire_exclusive_from_reserved(int fd) {
    // Acquire pending first, then try up to MAX_EXCLUSIVE_TRIES to acquire exclusive
    // If not a single attempt succesded, roll back the locks acquired from pending. Or should we keep?
    // Roll it back is cleaner

    // Acquire write on Pending
    bool acquired_pending = false;
    for (int i = 0; i < MAX_PENDING_RETRIES; i++) {
        LockMgrStatus pending_byte_write = acquire_primitive_lock(fd, PrimitiveLockType::WRITE, PENDING_BYTE);
        if (pending_byte_write == LockMgrStatus::Success) {
            acquired_pending = true;
            break;
        }
    }
    if (!acquired_pending) return LockMgrStatus::Busy;
    lock_state = LockState::PENDING;

    for (int i = 0; i < MAX_EXCLUSIVE_RETRIES; i++) {
        LockMgrStatus shared_byte_write = acquire_primitive_lock(fd, PrimitiveLockType::WRITE, SHARED_BYTE);
        if (shared_byte_write == LockMgrStatus::Success) {
            lock_state = LockState::EXCLUSIVE;
            return LockMgrStatus::Success;
        }
    }
    // Release the pending byte lock on failure
    if (release_primitive_lock(fd, PENDING_BYTE) != LockMgrStatus::Success) {
        std::abort();
    }
    lock_state = LockState::RESERVED;
    return LockMgrStatus::Busy;
}

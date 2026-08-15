#pragma once

#include <Log/Config.h>
#include <Log/Segment.h>
#include <Log/WalRecord.h>

#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

class Log {
public:
    explicit Log(Config config);
    ~Log() noexcept;

    Log(const Log&) = delete;
    Log& operator=(const Log&) = delete;
    Log(Log&&) = delete;
    Log& operator=(Log&&) = delete;

    void open(const std::string& directory);
    void close();
    bool is_open() const noexcept;

    Lsn append(PendingWalRecord record);
    WalRecord read(Lsn lsn) const;
    std::vector<WalRecord> scan() const;
    void sync_through(Lsn target_lsn);

    Lsn next_lsn() const noexcept;
    Lsn durable_lsn() const noexcept;
    bool recovery_required() const noexcept;

private:
    Config config_;
    std::string directory_;
    std::vector<std::unique_ptr<Segment>> segments_;
    Lsn next_lsn_ = 0;
    Lsn durable_lsn_ = 0;
    bool recovery_required_ = false;
    mutable std::shared_mutex mutex_;

    void create_segment(Lsn base_lsn);
};

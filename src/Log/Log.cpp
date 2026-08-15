#include <Log/Log.h>

#include <DiskIO.h>

#include <fcntl.h>
#include <filesystem>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace {

struct SegmentFiles { bool store = false; bool index = false; };

std::string segment_stem(Lsn base_lsn) {
    std::ostringstream name;
    name << "segment-" << std::setw(20) << std::setfill('0') << base_lsn;
    return name.str();
}

std::pair<Lsn, bool> parse_segment_name(const std::string& name) {
    constexpr std::size_t prefix_size = 8;
    constexpr std::size_t digits_size = 20;
    const bool store = name.size() == prefix_size + digits_size + 6 && name.ends_with(".store");
    const bool index = name.size() == prefix_size + digits_size + 6 && name.ends_with(".index");
    if (!name.starts_with("segment-")) return {0, false};
    if (!store && !index) throw std::runtime_error("Malformed WAL segment filename");
    const auto digits = name.substr(prefix_size, digits_size);
    if (digits.find_first_not_of("0123456789") != std::string::npos) throw std::runtime_error("Malformed WAL segment filename");
    try { return {std::stoull(digits), store}; }
    catch (...) { throw std::runtime_error("Malformed WAL segment filename"); }
}

} // namespace

Log::Log(Config config) : config_(config) { config_.validate(); }

Log::~Log() noexcept {
    std::unique_lock lock(mutex_);
    segments_.clear();
}

void Log::open(const std::string& directory) {
    if (directory.empty()) throw std::invalid_argument("WAL directory must not be empty");
    std::unique_lock lock(mutex_);
    if (!segments_.empty()) throw std::runtime_error("Log is already open");

    try {
        std::filesystem::create_directories(directory);
        directory_ = directory;
        std::map<Lsn, SegmentFiles> discovered;
        for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
            const auto name = entry.path().filename().string();
            if (!name.starts_with("segment-")) continue;
            const auto [base, is_store] = parse_segment_name(name);
            if (base == 0) throw std::runtime_error("Segment base LSN zero is invalid");
            auto& files = discovered[base];
            (is_store ? files.store : files.index) = true;
        }

        if (discovered.empty()) {
            create_segment(config_.initial_lsn);
        } else {
            Lsn expected_base = config_.initial_lsn;
            for (const auto& [base, files] : discovered) {
                if (base != expected_base) throw std::runtime_error("WAL segments do not form a continuous LSN sequence");
                if (!files.store) throw std::runtime_error("WAL Index exists without its authoritative Store");
                const auto stem = std::filesystem::path(directory_) / segment_stem(base);
                const int store_fd = disk::open_file(stem.string() + ".store", O_RDWR);
                int index_fd = -1;
                try {
                    index_fd = disk::open_file(stem.string() + ".index", O_RDWR | O_CREAT, 0644);
                    segments_.push_back(std::make_unique<Segment>(base, store_fd, index_fd, config_));
                } catch (...) {
                    if (index_fd == -1) disk::close_file(store_fd);
                    throw;
                }
                expected_base = segments_.back()->next_lsn();
            }
        }
        next_lsn_ = segments_.back()->next_lsn();
        durable_lsn_ = next_lsn_ - 1;
        recovery_required_ = false;
    } catch (...) {
        segments_.clear(); directory_.clear(); next_lsn_ = durable_lsn_ = 0;
        throw;
    }
}

void Log::close() {
    std::unique_lock lock(mutex_);
    if (segments_.empty()) return;
    if (recovery_required_) throw std::runtime_error("Log must be reopened and recovered before close can synchronize");
    for (auto& segment : segments_) segment->sync();
    durable_lsn_ = next_lsn_ - 1;
    segments_.clear(); directory_.clear(); next_lsn_ = durable_lsn_ = 0;
}

bool Log::is_open() const noexcept { std::shared_lock lock(mutex_); return !segments_.empty(); }

Lsn Log::append(PendingWalRecord pending) {
    std::unique_lock lock(mutex_);
    if (segments_.empty()) throw std::runtime_error("Log is not open");
    if (recovery_required_) throw std::runtime_error("Log must be reopened and recovered before appending");

    // A complete record that crosses a limit remains in its original segment.
    // Rollover happens before the following append.
    if (segments_.back()->is_maxed()) create_segment(next_lsn_);
    WalRecord record{.lsn = next_lsn_, .type = pending.type,
                     .transaction_id = pending.transaction_id,
                     .prev_lsn = pending.prev_lsn, .data = std::move(pending.data)};
    segments_.back()->append(record);
    next_lsn_ += 1;
    return record.lsn;
}

WalRecord Log::read(Lsn lsn) const {
    std::shared_lock lock(mutex_);
    if (segments_.empty()) throw std::runtime_error("Log is not open");
    if (lsn < config_.initial_lsn || lsn >= next_lsn_) throw std::out_of_range("LSN is not present in Log");
    for (const auto& segment : segments_) {
        if (lsn >= segment->base_lsn() && lsn < segment->next_lsn()) return segment->read(lsn);
    }
    throw std::out_of_range("LSN is not present in Log");
}

std::vector<WalRecord> Log::scan() const {
    std::shared_lock lock(mutex_);
    if (segments_.empty()) throw std::runtime_error("Log is not open");
    std::vector<WalRecord> records;
    for (const auto& segment : segments_) {
        auto part = segment->scan();
        records.insert(records.end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
    }
    return records;
}
void Log::sync_through(Lsn) { throw std::runtime_error("Log synchronization is not implemented"); }
Lsn Log::next_lsn() const noexcept { std::shared_lock lock(mutex_); return next_lsn_; }
Lsn Log::durable_lsn() const noexcept { std::shared_lock lock(mutex_); return durable_lsn_; }
bool Log::recovery_required() const noexcept { std::shared_lock lock(mutex_); return recovery_required_; }

void Log::create_segment(Lsn base_lsn) {
    const auto stem = std::filesystem::path(directory_) / segment_stem(base_lsn);
    const int store_fd = disk::open_file(stem.string() + ".store", O_RDWR | O_CREAT | O_EXCL, 0644);
    int index_fd = -1;
    try {
        index_fd = disk::open_file(stem.string() + ".index", O_RDWR | O_CREAT | O_EXCL, 0644);
        disk::sync_directory(directory_);
        segments_.push_back(std::make_unique<Segment>(base_lsn, store_fd, index_fd, config_));
    } catch (...) {
        if (index_fd == -1) disk::close_file(store_fd);
        throw;
    }
}

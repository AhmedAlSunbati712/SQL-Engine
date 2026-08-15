#include <gtest/gtest.h>

#include <Log/Log.h>

#include <filesystem>
#include <fstream>

namespace {
class TempDir {
public:
    TempDir() { path = std::filesystem::temp_directory_path() / std::filesystem::path("stoneleaf-log-XXXXXX");
        std::string value = path.string(); value.push_back('\0'); path = ::mkdtemp(value.data()); }
    ~TempDir() { std::filesystem::remove_all(path); }
    std::filesystem::path path;
};
Config config() { return {.max_index_bytes = 120, .max_store_bytes = 8192, .initial_lsn = 1}; }
PendingWalRecord system(std::vector<char> data = {}) {
    return {.type = WalRecordType::SystemAction, .data = std::move(data)};
}

TEST(LogTest, CreatesDirectoryAndInitialSegment) {
    TempDir parent; auto directory = parent.path / "wal";
    Log log(config()); log.open(directory.string());
    EXPECT_TRUE(log.is_open());
    EXPECT_TRUE(std::filesystem::exists(directory / "segment-00000000000000000001.store"));
    EXPECT_TRUE(std::filesystem::exists(directory / "segment-00000000000000000001.index"));
    log.close(); EXPECT_FALSE(log.is_open()); EXPECT_NO_THROW(log.close());
}

TEST(LogTest, RebuildsMissingIndexAndRejectsMissingStore) {
    TempDir dir;
    { std::ofstream(dir.path / "segment-00000000000000000001.store"); }
    Log recovered(config()); EXPECT_NO_THROW(recovered.open(dir.path.string())); recovered.close();
    std::filesystem::remove(dir.path / "segment-00000000000000000001.store");
    Log invalid(config()); EXPECT_THROW(invalid.open(dir.path.string()), std::runtime_error);
}

TEST(LogTest, RejectsMalformedNamesAndSegmentGaps) {
    TempDir malformed; { std::ofstream(malformed.path / "segment-nope.store"); }
    Log first(config()); EXPECT_THROW(first.open(malformed.path.string()), std::runtime_error);
    TempDir gap;
    { std::ofstream(gap.path / "segment-00000000000000000001.store"); std::ofstream(gap.path / "segment-00000000000000000001.index");
      std::ofstream(gap.path / "segment-00000000000000000003.store"); std::ofstream(gap.path / "segment-00000000000000000003.index"); }
    Log second(config()); EXPECT_THROW(second.open(gap.path.string()), std::runtime_error);
}

TEST(LogTest, AssignsDenseLsnsAndSupportsReadScanAndReopen) {
    TempDir dir;
    { Log log(config()); log.open(dir.path.string());
      EXPECT_EQ(log.append(system({'a'})), 1u); EXPECT_EQ(log.append(system({'b'})), 2u);
      EXPECT_EQ(log.read(2).data, (std::vector<char>{'b'}));
      auto records = log.scan(); ASSERT_EQ(records.size(), 2u); EXPECT_EQ(records[0].lsn, 1u); log.close(); }
    Log reopened(config()); reopened.open(dir.path.string());
    EXPECT_EQ(reopened.next_lsn(), 3u); EXPECT_EQ(reopened.append(system({'c'})), 3u);
}

TEST(LogTest, RollsBeforeAppendAfterARecordCrossesLimit) {
    TempDir dir; Config small = config(); small.max_store_bytes = 44;
    Log log(small); log.open(dir.path.string());
    EXPECT_EQ(log.append(system({'a'})), 1u);
    EXPECT_EQ(log.append(system({'b'})), 2u);
    EXPECT_TRUE(std::filesystem::exists(dir.path / "segment-00000000000000000002.store"));
    auto records = log.scan(); ASSERT_EQ(records.size(), 2u); EXPECT_EQ(records[1].lsn, 2u);
}
} // namespace

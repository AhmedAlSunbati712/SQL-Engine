#include <gtest/gtest.h>

#include <BTree.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace {

class TempDbPath {
    public:
        TempDbPath() {
            auto unique_suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            temp_dir = std::filesystem::temp_directory_path() / ("stoneleafdb_btree_unit_" + unique_suffix);
            db_path = temp_dir / "test.db";
            std::filesystem::create_directories(temp_dir);
        }

        ~TempDbPath() {
            std::error_code ec;
            std::filesystem::remove_all(temp_dir, ec);
        }

        std::filesystem::path temp_dir;
        std::filesystem::path db_path;
    };

TEST(BTreeUnitTest, CloseOnFreshTreeSucceeds) {
    BTree tree;

    EXPECT_EQ(tree.close(), BTreeStatus::Success);
}

TEST(BTreeUnitTest, CommitOnFreshTreeFails) {
    BTree tree;

    EXPECT_EQ(tree.commit(), BTreeCommitStatus::Failed);
}

TEST(BTreeUnitTest, RollbackOnFreshTreeFails) {
    BTree tree;

    EXPECT_EQ(tree.rollback(), BTreeRollbackStatus::Failed);
}

TEST(BTreeUnitTest, OpenFailsWhenPathIsDirectory) {
    TempDbPath temp_db;
    BTree tree;

    EXPECT_EQ(tree.open(temp_db.temp_dir.string()), BTreeStatus::FailedToOpenDB);
}

TEST(BTreeUnitTest, OpenThenCloseSucceeds) {
    TempDbPath temp_db;
    BTree tree;

    ASSERT_EQ(tree.open(temp_db.db_path.string()), BTreeStatus::Success);
    EXPECT_EQ(tree.close(), BTreeStatus::Success);
}

} // namespace

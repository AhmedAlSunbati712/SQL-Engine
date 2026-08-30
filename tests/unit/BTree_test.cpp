#include <gtest/gtest.h>

#include <BTree.h>
#include <LockManager/LockManager.h>
#include <Log/Log.h>
#include <TransactionManager/TransactionManager.h>

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

class TempDbPath {
    public:
        TempDbPath() {
            auto unique_suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            temp_dir = std::filesystem::temp_directory_path() / ("stoneleafdb_btree_unit_" + unique_suffix);
            db_path = temp_dir / "test.db";
            wal_path = temp_dir / "test.wal";
            std::filesystem::create_directories(temp_dir);
        }

        ~TempDbPath() {
            std::error_code ec;
            std::filesystem::remove_all(temp_dir, ec);
        }

        std::filesystem::path temp_dir;
        std::filesystem::path db_path;
        std::filesystem::path wal_path;
    };

class BTreeUndoExecutor final : public TransactionUndoExecutor {
public:
    BTree *tree = nullptr;

    void undo(
        Transaction &transaction,
        const UndoDescriptor &undo,
        CompensationAppender append_compensation
    ) override {
        if (!tree || !tree->apply_undo(
                transaction,
                undo,
                std::move(append_compensation))) {
            throw std::runtime_error("B-tree undo failed");
        }
    }
};

Config wal_config() {
    return {
        .max_index_bytes = 1000 * Index::ENTRY_SIZE,
        .max_store_bytes = 16 * 1024 * 1024,
        .initial_lsn = 1,
    };
}

// Owns the Log/LockManager/TransactionManager stack a BTree now requires
// before open() can succeed, since Pager only implements the WAL path.
class TransactionManagerStack {
public:
    explicit TransactionManagerStack(BTree &tree, const std::filesystem::path &wal_path)
        : log(wal_config()),
          transaction_manager(log, lock_manager, undo_executor) {
        log.open(wal_path.string());
        undo_executor.tree = &tree;
        tree.attach_transaction_manager(transaction_manager);
    }

    Log log;
    LockManager lock_manager;
    BTreeUndoExecutor undo_executor;
    TransactionManager transaction_manager;
};

} // namespace

TEST(BTreeUnitTest, CloseOnFreshTreeSucceeds) {
    BTree tree;

    EXPECT_EQ(tree.close(), BTreeStatus::Success);
}

TEST(BTreeUnitTest, OpenFailsWithoutAttachedTransactionManager) {
    TempDbPath temp_db;
    BTree tree;

    EXPECT_EQ(tree.open(temp_db.db_path.string()), BTreeStatus::FailedToOpenDB);
}

TEST(BTreeUnitTest, OpenFailsWhenPathIsDirectory) {
    TempDbPath temp_db;
    BTree tree;
    TransactionManagerStack stack(tree, temp_db.wal_path);

    EXPECT_EQ(tree.open(temp_db.temp_dir.string()), BTreeStatus::FailedToOpenDB);
}

TEST(BTreeUnitTest, OpenThenCloseSucceeds) {
    TempDbPath temp_db;
    BTree tree;
    TransactionManagerStack stack(tree, temp_db.wal_path);

    ASSERT_EQ(tree.open(temp_db.db_path.string()), BTreeStatus::Success);
    EXPECT_EQ(tree.close(), BTreeStatus::Success);
}

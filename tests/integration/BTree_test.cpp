#include <gtest/gtest.h>

#include <BTree.h>
#include <KeyCodec.h>
#include <LockManager/LockManager.h>
#include <Log/Log.h>
#include <Log/PendingBTreeAction.h>
#include <Log/WalRecords.h>
#include <TransactionManager/TransactionManager.h>
#include <V2PageCodec.h>
#include <ValueCodec.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace {

std::string value_to_string(const Value &value) {
    return std::string(value.data.begin(), value.data.end());
}

Key make_key(std::uint64_t key) {
    return KeyCodec::make_uint64(key);
}

class NoopUndoExecutor final : public TransactionUndoExecutor {
public:
    std::vector<PageEffect> undo(
        Transaction&,
        const UndoDescriptor&) override {
        return {};
    }
};

Config wal_config() {
    return {
        .max_index_bytes = 100 * Index::ENTRY_SIZE,
        .max_store_bytes = 1024 * 1024,
        .initial_lsn = 1,
    };
}

std::array<char, V2_PAGE_SIZE> read_page(
    const std::filesystem::path& path,
    std::uint32_t page_num
) {
    std::array<char, V2_PAGE_SIZE> bytes{};
    std::ifstream input(path, std::ios::binary);
    input.seekg(static_cast<std::streamoff>(page_num) * V2_PAGE_SIZE);
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

class BTreeIntegrationTest : public ::testing::Test {
    protected:
        void SetUp() override {
            auto unique_suffix = std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()
            );
            temp_dir = std::filesystem::temp_directory_path() / ("stoneleafdb_btree_test_" + unique_suffix);
            db_path = temp_dir / "test.db";
            std::filesystem::create_directories(temp_dir);
        }

        void TearDown() override {
            std::error_code ec;
            std::filesystem::remove_all(temp_dir, ec);
        }

        Value make_small_key_value(std::uint64_t key) {
            char payload = static_cast<char>('A' + (key % 26));
            return ValueCodec::make_char(std::string(1, payload));
        }

        void expect_get_value(BTree &tree, std::uint64_t key, const std::string &expected_payload) {
            BTreeGetStatus get_result = tree.get(make_key(key));
            ASSERT_EQ(get_result.status, BTreeStatus::Success);
            EXPECT_EQ(value_to_string(get_result.value), expected_payload);
        }

        void expect_key_missing(BTree &tree, std::uint64_t key) {
            BTreeGetStatus get_result = tree.get(make_key(key));
            EXPECT_EQ(get_result.status, BTreeStatus::KeyNotInTree);
        }

        std::filesystem::path temp_dir;
        std::filesystem::path db_path;
};

TEST_F(BTreeIntegrationTest, GetOnFreshTreeReturnsKeyNotInTree) {
    BTree tree;
    ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);

    BTreeGetStatus get_result = tree.get(make_key(42));
    EXPECT_EQ(get_result.status, BTreeStatus::KeyNotInTree);
}

TEST_F(BTreeIntegrationTest, InsertCommitAndReopenPreservesSingleKey) {
    {
        BTree tree;
        ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);

        Value value = ValueCodec::make_char("A");
        ASSERT_EQ(tree.insert(make_key(7), value), BTreeStatus::Success);
        ASSERT_EQ(tree.commit(), BTreeCommitStatus::Success);
    }

    BTree reopened_tree;
    ASSERT_EQ(reopened_tree.open(db_path.string()), BTreeStatus::Success);
    expect_get_value(reopened_tree, 7, "A");
}

TEST_F(BTreeIntegrationTest, MutationActionCollectsCompleteDeduplicatedEffects) {
    BTree tree;
    ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);

    Key key = make_key(7);
    Value value = ValueCodec::make_char("A");
    PendingBTreeAction action(1, 1);
    action.set_undo(InsertUndo{key});

    ASSERT_EQ(tree.insert(key, value, action), BTreeStatus::Success);
    ASSERT_EQ(action.effects().size(), 2U);

    const PageEffect *header_effect = nullptr;
    const PageEffect *root_effect = nullptr;
    for (const PageEffect& effect : action.effects()) {
        if (effect.page_num == 0) header_effect = &effect;
        if (effect.page_num == 1) root_effect = &effect;
    }

    ASSERT_NE(header_effect, nullptr);
    ASSERT_NE(root_effect, nullptr);
    EXPECT_EQ(header_effect->kind, PageEffectKind::Write);
    EXPECT_EQ(root_effect->kind, PageEffectKind::Allocate);
    EXPECT_EQ(V2PageCodec::page_num(header_effect->after_image), 0U);
    EXPECT_EQ(V2PageCodec::page_num(root_effect->after_image), 1U);
}

TEST_F(BTreeIntegrationTest, TransactionalInsertAppendsWalAndInstallsAssignedLsn) {
    const std::filesystem::path wal_path = temp_dir / "wal";
    Log log(wal_config());
    log.open(wal_path.string());
    LockManager lock_manager;
    NoopUndoExecutor undo_executor;
    TransactionManager transaction_manager(log, lock_manager, undo_executor);

    BTree tree;
    ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);
    tree.attach_transaction_manager(transaction_manager);

    const TransactionHandle transaction = transaction_manager.begin();
    Key key = make_key(7);
    Value value = ValueCodec::make_char("A");
    PendingBTreeAction action(transaction->id(), transaction->last_lsn());
    action.set_undo(InsertUndo{key});

    ASSERT_EQ(
        tree.insert(transaction, key, value, action),
        BTreeStatus::Success);
    const Lsn action_lsn = transaction->last_lsn();
    ASSERT_EQ(action_lsn, 2U);

    const WalRecord record = log.read(action_lsn);
    EXPECT_EQ(record.type, WalRecordType::BTreeAction);
    EXPECT_EQ(record.prev_lsn, 1U);
    EXPECT_EQ(
        std::get<BTreeActionPayload>(WalRecords::decode(record)).effects.size(),
        2U);

    // The legacy pager commit remains in place during this integration slice.
    // Flush it so the test can inspect the installed LSNs on disk.
    ASSERT_EQ(tree.commit(), BTreeCommitStatus::Success);
    EXPECT_GE(log.durable_lsn(), action_lsn);
    const auto header = read_page(db_path, 0);
    const auto root = read_page(db_path, 1);
    EXPECT_EQ(V2PageCodec::page_lsn(header), action_lsn);
    EXPECT_EQ(V2PageCodec::page_lsn(root), action_lsn);
    EXPECT_EQ(V2PageCodec::validate(header), V2PageCodecResult::Success);
    EXPECT_EQ(V2PageCodec::validate(root), V2PageCodecResult::Success);

    EXPECT_EQ(
        transaction_manager.commit(transaction),
        CommitStatus::Success);
}

TEST_F(BTreeIntegrationTest, OverwriteExistingKeyCommitAndReopenPreservesUpdatedValue) {
    {
        BTree tree;
        ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);

        Value initial_value = ValueCodec::make_char("F");
        ASSERT_EQ(tree.insert(make_key(11), initial_value), BTreeStatus::Success);
        ASSERT_EQ(tree.commit(), BTreeCommitStatus::Success);
    }

    {
        BTree tree;
        ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);

        Value updated_value = ValueCodec::make_char("T");
        ASSERT_EQ(tree.insert(make_key(11), updated_value), BTreeStatus::Success);
        ASSERT_EQ(tree.commit(), BTreeCommitStatus::Success);
    }

    BTree reopened_tree;
    ASSERT_EQ(reopened_tree.open(db_path.string()), BTreeStatus::Success);
    expect_get_value(reopened_tree, 11, "T");
}

TEST_F(BTreeIntegrationTest, RollbackDiscardsUncommittedInsert) {
    BTree tree;
    ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);

    Value value = ValueCodec::make_char("R");
    ASSERT_EQ(tree.insert(make_key(25), value), BTreeStatus::Success);
    ASSERT_EQ(tree.rollback(), BTreeRollbackStatus::Success);

    expect_key_missing(tree, 25);
}

TEST_F(BTreeIntegrationTest, ManyInsertsSplitRootAndRemainReadableAcrossReopen) {
    {
        BTree tree;
        ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);

        for (std::uint64_t key = 0; key < 260; key++) {
            Value value = make_small_key_value(key);
            ASSERT_EQ(tree.insert(make_key(key), value), BTreeStatus::Success);
        }

        ASSERT_EQ(tree.commit(), BTreeCommitStatus::Success);
    }

    BTree reopened_tree;
    ASSERT_EQ(reopened_tree.open(db_path.string()), BTreeStatus::Success);
    for (std::uint64_t key = 0; key < 260; key++) {
        expect_get_value(reopened_tree, key, std::string(1, static_cast<char>('A' + (key % 26))));
    }
}

TEST_F(BTreeIntegrationTest, RemoveCommitAndReopenDeletesKey) {
    {
        BTree tree;
        ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);

        Value left_value = ValueCodec::make_char("L");
        Value right_value = ValueCodec::make_char("R");
        ASSERT_EQ(tree.insert(make_key(10), left_value), BTreeStatus::Success);
        ASSERT_EQ(tree.insert(make_key(20), right_value), BTreeStatus::Success);
        ASSERT_EQ(tree.commit(), BTreeCommitStatus::Success);
    }

    {
        BTree tree;
        ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);

        BTreeRemoveStatus remove_result = tree.remove(make_key(10));
        ASSERT_EQ(remove_result.status, BTreeStatus::Success);
        EXPECT_EQ(value_to_string(remove_result.value), "L");
        ASSERT_EQ(tree.commit(), BTreeCommitStatus::Success);
    }

    BTree reopened_tree;
    ASSERT_EQ(reopened_tree.open(db_path.string()), BTreeStatus::Success);
    expect_key_missing(reopened_tree, 10);
    expect_get_value(reopened_tree, 20, "R");
}

TEST_F(BTreeIntegrationTest, RollbackDiscardsRemove) {
    {
        BTree tree;
        ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);

        Value value = ValueCodec::make_char("T");
        ASSERT_EQ(tree.insert(make_key(55), value), BTreeStatus::Success);
        ASSERT_EQ(tree.commit(), BTreeCommitStatus::Success);
    }

    BTree tree;
    ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);

    BTreeRemoveStatus remove_result = tree.remove(make_key(55));
    ASSERT_EQ(remove_result.status, BTreeStatus::Success);
    ASSERT_EQ(tree.rollback(), BTreeRollbackStatus::Success);

    expect_get_value(tree, 55, "T");
}

TEST_F(BTreeIntegrationTest, ManyDeletesTriggerStructuralRepairAndKeepRemainingKeysReadable) {
    {
        BTree tree;
        ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);

        for (std::uint64_t key = 0; key < 260; key++) {
            Value value = make_small_key_value(key);
            ASSERT_EQ(tree.insert(make_key(key), value), BTreeStatus::Success);
        }
        ASSERT_EQ(tree.commit(), BTreeCommitStatus::Success);
    }

    {
        BTree tree;
        ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);

        for (std::uint64_t key = 0; key < 180; key++) {
            BTreeRemoveStatus remove_result = tree.remove(make_key(key));
            ASSERT_EQ(remove_result.status, BTreeStatus::Success);
            EXPECT_EQ(remove_result.value.type, ValueType::Char);
            EXPECT_EQ(value_to_string(remove_result.value), std::string(1, static_cast<char>('A' + (key % 26))));
        }

        ASSERT_EQ(tree.commit(), BTreeCommitStatus::Success);
    }

    BTree reopened_tree;
    ASSERT_EQ(reopened_tree.open(db_path.string()), BTreeStatus::Success);

    for (std::uint64_t key = 0; key < 180; key++) {
        expect_key_missing(reopened_tree, key);
    }

    for (std::uint64_t key = 180; key < 260; key++) {
        expect_get_value(reopened_tree, key, std::string(1, static_cast<char>('A' + (key % 26))));
    }
}

TEST_F(BTreeIntegrationTest, HeterogeneousKeysRemainDistinctAcrossCommit) {
    std::vector<Key> keys = {
        KeyCodec::make_bool(true),
        KeyCodec::make_uint64(1),
        KeyCodec::make_int64(1),
        KeyCodec::make_string("1"),
        KeyCodec::make_bytes({'1'})
    };

    {
        BTree tree;
        ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);

        for (std::size_t i = 0; i < keys.size(); i++) {
            Value value = ValueCodec::make_varuint(i);
            ASSERT_EQ(tree.insert(keys[i], value), BTreeStatus::Success);
        }

        ASSERT_EQ(tree.commit(), BTreeCommitStatus::Success);
    }

    BTree reopened_tree;
    ASSERT_EQ(reopened_tree.open(db_path.string()), BTreeStatus::Success);

    for (std::size_t i = 0; i < keys.size(); i++) {
        BTreeGetStatus get_result = reopened_tree.get(keys[i]);
        ASSERT_EQ(get_result.status, BTreeStatus::Success);

        std::uint64_t decoded_value = 0;
        ASSERT_TRUE(ValueCodec::decode_varuint(get_result.value, &decoded_value));
        EXPECT_EQ(decoded_value, i);
    }
}

} // namespace

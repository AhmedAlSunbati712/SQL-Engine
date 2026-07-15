#include <gtest/gtest.h>

#include <BTree.h>
#include <KeyCodec.h>
#include <ValueCodec.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace {

std::string value_to_string(const Value &value) {
    return std::string(value.data.begin(), value.data.end());
}

Key make_key(std::uint64_t key) {
    return keycodec::make_uint64(key);
}

class BTreeIntegrationTest : public ::testing::Test {
    protected:
        void SetUp() override {
            auto unique_suffix = std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()
            );
            temp_dir = std::filesystem::temp_directory_path() / ("sqlengine_btree_test_" + unique_suffix);
            db_path = temp_dir / "test.db";
            std::filesystem::create_directories(temp_dir);
        }

        void TearDown() override {
            std::error_code ec;
            std::filesystem::remove_all(temp_dir, ec);
        }

        Value make_small_key_value(std::uint64_t key) {
            char payload = static_cast<char>('A' + (key % 26));
            return valuecodec::make_char(std::string(1, payload));
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

        Value value = valuecodec::make_char("A");
        ASSERT_EQ(tree.insert(make_key(7), value), BTreeStatus::Success);
        ASSERT_EQ(tree.commit(), BTreeCommitStatus::Success);
    }

    BTree reopened_tree;
    ASSERT_EQ(reopened_tree.open(db_path.string()), BTreeStatus::Success);
    expect_get_value(reopened_tree, 7, "A");
}

TEST_F(BTreeIntegrationTest, OverwriteExistingKeyCommitAndReopenPreservesUpdatedValue) {
    {
        BTree tree;
        ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);

        Value initial_value = valuecodec::make_char("F");
        ASSERT_EQ(tree.insert(make_key(11), initial_value), BTreeStatus::Success);
        ASSERT_EQ(tree.commit(), BTreeCommitStatus::Success);
    }

    {
        BTree tree;
        ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);

        Value updated_value = valuecodec::make_char("T");
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

    Value value = valuecodec::make_char("R");
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

        Value left_value = valuecodec::make_char("L");
        Value right_value = valuecodec::make_char("R");
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

        Value value = valuecodec::make_char("T");
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
        keycodec::make_bool(true),
        keycodec::make_uint64(1),
        keycodec::make_int64(1),
        keycodec::make_string("1"),
        keycodec::make_bytes({'1'})
    };

    {
        BTree tree;
        ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);

        for (std::size_t i = 0; i < keys.size(); i++) {
            Value value = valuecodec::make_varuint(i);
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
        ASSERT_TRUE(valuecodec::decode_varuint(get_result.value, &decoded_value));
        EXPECT_EQ(decoded_value, i);
    }
}

} // namespace

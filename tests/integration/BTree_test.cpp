#include <gtest/gtest.h>

#include <BTree.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

namespace {

Value make_value(ValueType type, const std::string &payload) {
    Value value{};
    value.type = type;
    value.size = static_cast<std::uint32_t>(payload.size());
    value.data.assign(payload.begin(), payload.end());
    return value;
}

std::string value_to_string(const Value &value) {
    return std::string(value.data.begin(), value.data.end());
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

        Value make_key_value(std::uint64_t key) {
            return make_value(ValueType::VarInt, "value-" + std::to_string(key));
        }

        Value make_small_key_value(std::uint64_t key) {
            char payload = static_cast<char>('A' + (key % 26));
            return make_value(ValueType::Char, std::string(1, payload));
        }

        void expect_get_value(BTree &tree, std::uint64_t key, const std::string &expected_payload) {
            BTreeGetStatus get_result = tree.get(key);
            ASSERT_EQ(get_result.status, BTreeStatus::Success);
            EXPECT_EQ(value_to_string(get_result.value), expected_payload);
        }

        void expect_key_missing(BTree &tree, std::uint64_t key) {
            BTreeGetStatus get_result = tree.get(key);
            EXPECT_EQ(get_result.status, BTreeStatus::KeyNotInTree);
        }

        std::filesystem::path temp_dir;
        std::filesystem::path db_path;
};

TEST_F(BTreeIntegrationTest, GetOnFreshTreeReturnsKeyNotInTree) {
    BTree tree;
    ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);

    BTreeGetStatus get_result = tree.get(42);
    EXPECT_EQ(get_result.status, BTreeStatus::KeyNotInTree);
}

TEST_F(BTreeIntegrationTest, InsertCommitAndReopenPreservesSingleKey) {
    {
        BTree tree;
        ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);

        Value value = make_value(ValueType::Char, "A");
        ASSERT_EQ(tree.insert(7, value), BTreeStatus::Success);
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

        Value initial_value = make_value(ValueType::Bool, "F");
        ASSERT_EQ(tree.insert(11, initial_value), BTreeStatus::Success);
        ASSERT_EQ(tree.commit(), BTreeCommitStatus::Success);
    }

    {
        BTree tree;
        ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);

        Value updated_value = make_value(ValueType::Bool, "T");
        ASSERT_EQ(tree.insert(11, updated_value), BTreeStatus::Success);
        ASSERT_EQ(tree.commit(), BTreeCommitStatus::Success);
    }

    BTree reopened_tree;
    ASSERT_EQ(reopened_tree.open(db_path.string()), BTreeStatus::Success);
    expect_get_value(reopened_tree, 11, "T");
}

TEST_F(BTreeIntegrationTest, RollbackDiscardsUncommittedInsert) {
    BTree tree;
    ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);

    Value value = make_value(ValueType::Char, "R");
    ASSERT_EQ(tree.insert(25, value), BTreeStatus::Success);
    ASSERT_EQ(tree.rollback(), BTreeRollbackStatus::Success);

    expect_key_missing(tree, 25);
}

TEST_F(BTreeIntegrationTest, ManyInsertsSplitRootAndRemainReadableAcrossReopen) {
    {
        BTree tree;
        ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);

        for (std::uint64_t key = 0; key < 260; key++) {
            Value value = make_small_key_value(key);
            ASSERT_EQ(tree.insert(key, value), BTreeStatus::Success);
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

        Value left_value = make_value(ValueType::Char, "L");
        Value right_value = make_value(ValueType::Char, "R");
        ASSERT_EQ(tree.insert(10, left_value), BTreeStatus::Success);
        ASSERT_EQ(tree.insert(20, right_value), BTreeStatus::Success);
        ASSERT_EQ(tree.commit(), BTreeCommitStatus::Success);
    }

    {
        BTree tree;
        ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);

        BTreeRemoveStatus remove_result = tree.remove(10);
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

        Value value = make_value(ValueType::Bool, "T");
        ASSERT_EQ(tree.insert(55, value), BTreeStatus::Success);
        ASSERT_EQ(tree.commit(), BTreeCommitStatus::Success);
    }

    BTree tree;
    ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);

    BTreeRemoveStatus remove_result = tree.remove(55);
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
            ASSERT_EQ(tree.insert(key, value), BTreeStatus::Success);
        }
        ASSERT_EQ(tree.commit(), BTreeCommitStatus::Success);
    }

    {
        BTree tree;
        ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);

        for (std::uint64_t key = 0; key < 180; key++) {
            BTreeRemoveStatus remove_result = tree.remove(key);
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

} // namespace

#include <gtest/gtest.h>

#include <BTreeCursor.h>
#include <KeyCodec.h>
#include <ValueCodec.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

class BTreeCursorIntegrationTest : public ::testing::Test {
    protected:
        void SetUp() override {
            auto unique_suffix = std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()
            );
            temp_dir = std::filesystem::temp_directory_path() / ("sqlengine_cursor_test_" + unique_suffix);
            db_path = temp_dir / "test.db";
            std::filesystem::create_directories(temp_dir);
        }

        void TearDown() override {
            std::error_code ec;
            std::filesystem::remove_all(temp_dir, ec);
        }

        void insert_uint_keys(BTree &tree, std::uint64_t begin, std::uint64_t end, std::uint64_t step = 1) {
            for (std::uint64_t key = begin; key < end; key += step) {
                Value value = valuecodec::make_varuint(key);
                ASSERT_EQ(tree.insert(keycodec::make_uint64(key), value), BTreeStatus::Success);
            }
        }

        void expect_current(BTreeCursor &cursor, const Key &key, std::uint64_t value) {
            BTreeCursorResult result = cursor.current();
            ASSERT_EQ(result.status, BTreeCursorStatus::Success);
            EXPECT_TRUE(keycodec::equal(result.key, key));

            std::uint64_t decoded_value = 0;
            ASSERT_TRUE(valuecodec::decode_varuint(result.value, &decoded_value));
            EXPECT_EQ(decoded_value, value);
        }

        std::filesystem::path temp_dir;
        std::filesystem::path db_path;
};

TEST_F(BTreeCursorIntegrationTest, EmptyTreeUsesStickyEndState) {
    BTree tree;
    ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);

    BTreeCursor cursor = tree.open_cursor();
    EXPECT_FALSE(cursor.valid());
    EXPECT_EQ(cursor.current().status, BTreeCursorStatus::NotPositioned);
    EXPECT_EQ(cursor.next(), BTreeCursorStatus::NotPositioned);

    EXPECT_EQ(cursor.seek_first(), BTreeCursorStatus::EndOfTree);
    EXPECT_FALSE(cursor.valid());
    EXPECT_EQ(cursor.current().status, BTreeCursorStatus::EndOfTree);
    EXPECT_EQ(cursor.next(), BTreeCursorStatus::EndOfTree);

    EXPECT_EQ(cursor.seek(keycodec::make_uint64(10)), BTreeCursorStatus::EndOfTree);
    EXPECT_EQ(cursor.current().status, BTreeCursorStatus::EndOfTree);
}

TEST_F(BTreeCursorIntegrationTest, SeekAndNextNavigateSingleLeaf) {
    BTree tree;
    ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);
    insert_uint_keys(tree, 10, 40, 10);
    ASSERT_EQ(tree.commit(), BTreeCommitStatus::Success);

    BTreeCursor cursor = tree.open_cursor();
    ASSERT_EQ(cursor.seek_first(), BTreeCursorStatus::Success);
    EXPECT_TRUE(cursor.valid());
    expect_current(cursor, keycodec::make_uint64(10), 10);

    ASSERT_EQ(cursor.next(), BTreeCursorStatus::Success);
    expect_current(cursor, keycodec::make_uint64(20), 20);

    ASSERT_EQ(cursor.seek(keycodec::make_uint64(25)), BTreeCursorStatus::Success);
    expect_current(cursor, keycodec::make_uint64(30), 30);

    ASSERT_EQ(cursor.seek(keycodec::make_uint64(5)), BTreeCursorStatus::Success);
    expect_current(cursor, keycodec::make_uint64(10), 10);

    EXPECT_EQ(cursor.seek(keycodec::make_uint64(40)), BTreeCursorStatus::EndOfTree);
    EXPECT_EQ(cursor.current().status, BTreeCursorStatus::EndOfTree);

    ASSERT_EQ(cursor.seek(keycodec::make_uint64(20)), BTreeCursorStatus::Success);
    expect_current(cursor, keycodec::make_uint64(20), 20);
}

TEST_F(BTreeCursorIntegrationTest, SeekCrossesLeafBoundaryForMissingKey) {
    BTree tree;
    ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);
    insert_uint_keys(tree, 0, 520, 2);
    ASSERT_EQ(tree.commit(), BTreeCommitStatus::Success);

    BTreeCursor cursor = tree.open_cursor();
    ASSERT_EQ(cursor.seek(keycodec::make_uint64(199)), BTreeCursorStatus::Success);
    expect_current(cursor, keycodec::make_uint64(200), 200);
}

TEST_F(BTreeCursorIntegrationTest, ScanCrossesLeafBoundaryInSortedOrder) {
    BTree tree;
    ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);
    insert_uint_keys(tree, 0, 260);
    ASSERT_EQ(tree.commit(), BTreeCommitStatus::Success);

    BTreeCursor cursor = tree.open_cursor();
    ASSERT_EQ(cursor.seek_first(), BTreeCursorStatus::Success);

    for (std::uint64_t key = 0; key < 260; key++) {
        expect_current(cursor, keycodec::make_uint64(key), key);
        BTreeCursorStatus expected_status = (key == 259)
            ? BTreeCursorStatus::EndOfTree
            : BTreeCursorStatus::Success;
        EXPECT_EQ(cursor.next(), expected_status);
    }

    EXPECT_FALSE(cursor.valid());
    EXPECT_EQ(cursor.current().status, BTreeCursorStatus::EndOfTree);
    EXPECT_EQ(cursor.next(), BTreeCursorStatus::EndOfTree);
}

TEST_F(BTreeCursorIntegrationTest, ScanUsesHeterogeneousKeyOrdering) {
    std::vector<Key> keys = {
        keycodec::make_string("z"),
        keycodec::make_int64(3),
        keycodec::make_bool(true),
        keycodec::make_bytes({'a'}),
        keycodec::make_uint64(10),
        keycodec::make_string("a"),
        keycodec::make_bool(false),
        keycodec::make_int64(-5),
        keycodec::make_uint64(1)
    };

    BTree tree;
    ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);
    for (std::size_t i = 0; i < keys.size(); i++) {
        Value value = valuecodec::make_varuint(i);
        ASSERT_EQ(tree.insert(keys[i], value), BTreeStatus::Success);
    }
    ASSERT_EQ(tree.commit(), BTreeCommitStatus::Success);

    std::vector<Key> expected_keys = keys;
    std::sort(expected_keys.begin(), expected_keys.end(), [](const Key &lhs, const Key &rhs) {
        return keycodec::compare(lhs, rhs) < 0;
    });

    BTreeCursor cursor = tree.open_cursor();
    ASSERT_EQ(cursor.seek_first(), BTreeCursorStatus::Success);
    for (std::size_t i = 0; i < expected_keys.size(); i++) {
        BTreeCursorResult result = cursor.current();
        ASSERT_EQ(result.status, BTreeCursorStatus::Success);
        EXPECT_TRUE(keycodec::equal(result.key, expected_keys[i]));

        BTreeCursorStatus expected_status = (i + 1 == expected_keys.size())
            ? BTreeCursorStatus::EndOfTree
            : BTreeCursorStatus::Success;
        EXPECT_EQ(cursor.next(), expected_status);
    }
}

TEST_F(BTreeCursorIntegrationTest, CurrentReturnsIndependentCopies) {
    BTree tree;
    ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);
    insert_uint_keys(tree, 7, 8);
    ASSERT_EQ(tree.commit(), BTreeCommitStatus::Success);

    BTreeCursor cursor = tree.open_cursor();
    ASSERT_EQ(cursor.seek_first(), BTreeCursorStatus::Success);

    BTreeCursorResult first_result = cursor.current();
    ASSERT_EQ(first_result.status, BTreeCursorStatus::Success);
    first_result.key.data.clear();
    first_result.value.data.clear();

    expect_current(cursor, keycodec::make_uint64(7), 7);
}

TEST_F(BTreeCursorIntegrationTest, ActiveCursorBlocksMutationsAndTransactionBoundaries) {
    BTree tree;
    ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);
    insert_uint_keys(tree, 10, 11);
    ASSERT_EQ(tree.commit(), BTreeCommitStatus::Success);

    BTreeCursor cursor = tree.open_cursor();
    EXPECT_EQ(tree.get(keycodec::make_uint64(10)).status, BTreeStatus::Success);

    Value value = valuecodec::make_varuint(20);
    EXPECT_EQ(tree.insert(keycodec::make_uint64(20), value), BTreeStatus::CursorActive);
    EXPECT_EQ(tree.remove(keycodec::make_uint64(10)).status, BTreeStatus::CursorActive);
    EXPECT_EQ(tree.commit(), BTreeCommitStatus::CursorActive);
    EXPECT_EQ(tree.rollback(), BTreeRollbackStatus::CursorActive);
    EXPECT_EQ(tree.close(), BTreeStatus::CursorActive);

    ASSERT_EQ(cursor.close(), BTreeCursorStatus::Success);
    EXPECT_EQ(tree.insert(keycodec::make_uint64(20), value), BTreeStatus::Success);
    EXPECT_EQ(tree.rollback(), BTreeRollbackStatus::Success);
}

TEST_F(BTreeCursorIntegrationTest, CloseIsIdempotentAndClosedCursorRejectsNavigation) {
    BTree tree;
    ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);

    BTreeCursor cursor = tree.open_cursor();
    EXPECT_EQ(cursor.close(), BTreeCursorStatus::Success);
    EXPECT_EQ(cursor.close(), BTreeCursorStatus::Success);
    EXPECT_FALSE(cursor.valid());
    EXPECT_EQ(cursor.seek_first(), BTreeCursorStatus::Closed);
    EXPECT_EQ(cursor.seek(keycodec::make_uint64(1)), BTreeCursorStatus::Closed);
    EXPECT_EQ(cursor.next(), BTreeCursorStatus::Closed);
    EXPECT_EQ(cursor.current().status, BTreeCursorStatus::Closed);
    EXPECT_EQ(tree.close(), BTreeStatus::Success);
}

TEST_F(BTreeCursorIntegrationTest, AllCursorsMustCloseBeforeWritesResume) {
    BTree tree;
    ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);

    BTreeCursor first = tree.open_cursor();
    BTreeCursor second = tree.open_cursor();
    Value value = valuecodec::make_varuint(1);

    ASSERT_EQ(first.close(), BTreeCursorStatus::Success);
    EXPECT_EQ(tree.insert(keycodec::make_uint64(1), value), BTreeStatus::CursorActive);

    ASSERT_EQ(second.close(), BTreeCursorStatus::Success);
    EXPECT_EQ(tree.insert(keycodec::make_uint64(1), value), BTreeStatus::Success);
    EXPECT_EQ(tree.rollback(), BTreeRollbackStatus::Success);
}

TEST_F(BTreeCursorIntegrationTest, DestructorUnregistersCursor) {
    BTree tree;
    ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);

    {
        BTreeCursor cursor = tree.open_cursor();
        EXPECT_EQ(tree.close(), BTreeStatus::CursorActive);
    }

    EXPECT_EQ(tree.close(), BTreeStatus::Success);
}

TEST_F(BTreeCursorIntegrationTest, MoveConstructionTransfersRegistrationAndPosition) {
    BTree tree;
    ASSERT_EQ(tree.open(db_path.string()), BTreeStatus::Success);
    insert_uint_keys(tree, 5, 6);
    ASSERT_EQ(tree.commit(), BTreeCommitStatus::Success);

    BTreeCursor source = tree.open_cursor();
    ASSERT_EQ(source.seek_first(), BTreeCursorStatus::Success);

    BTreeCursor moved(std::move(source));
    EXPECT_EQ(source.current().status, BTreeCursorStatus::Closed);
    expect_current(moved, keycodec::make_uint64(5), 5);

    Value value = valuecodec::make_varuint(6);
    EXPECT_EQ(tree.insert(keycodec::make_uint64(6), value), BTreeStatus::CursorActive);
    ASSERT_EQ(moved.close(), BTreeCursorStatus::Success);
    EXPECT_EQ(tree.insert(keycodec::make_uint64(6), value), BTreeStatus::Success);
    EXPECT_EQ(tree.rollback(), BTreeRollbackStatus::Success);
}

TEST_F(BTreeCursorIntegrationTest, MoveAssignmentTransfersRegistrationAcrossTrees) {
    std::filesystem::path second_db_path = temp_dir / "second.db";
    BTree first_tree;
    BTree second_tree;
    ASSERT_EQ(first_tree.open(db_path.string()), BTreeStatus::Success);
    ASSERT_EQ(second_tree.open(second_db_path.string()), BTreeStatus::Success);

    BTreeCursor source = first_tree.open_cursor();
    BTreeCursor destination = second_tree.open_cursor();
    destination = std::move(source);

    EXPECT_EQ(source.current().status, BTreeCursorStatus::Closed);
    EXPECT_EQ(second_tree.close(), BTreeStatus::Success);
    EXPECT_EQ(first_tree.close(), BTreeStatus::CursorActive);

    EXPECT_EQ(destination.close(), BTreeCursorStatus::Success);
    EXPECT_EQ(first_tree.close(), BTreeStatus::Success);
}

} // namespace

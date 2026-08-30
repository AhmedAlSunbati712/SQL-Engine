#include <gtest/gtest.h>

#include <KeyCodec.h>
#include <KeyStore.h>
#include <LockManager/LockManager.h>
#include <Log/Log.h>
#include <Log/WalRecords.h>
#include <ValueCodec.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace {

Config key_store_wal_config() {
    return {
        .max_index_bytes = 1000 * Index::ENTRY_SIZE,
        .max_store_bytes = 16 * 1024 * 1024,
        .initial_lsn = 1,
    };
}

Key encode_key(const KeyInput &input) {
    return KeyCodec::encode(input).value();
}

Value encode_value(const ValueInput &input) {
    return ValueCodec::encode(input).value();
}

void expect_value_input(const Value &value, const ValueInput &expected) {
    std::optional<ValueInput> decoded = ValueCodec::decode(value);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, expected);
}

std::vector<KeyStoreEntry> drain_cursor(KeyStoreCursor &cursor) {
    std::vector<KeyStoreEntry> entries;

    while (true) {
        KeyStoreCursorResult current = cursor.current();
        if (current.status == KeyStoreCursorStatus::EndOfScan) break;
        if (current.status != KeyStoreCursorStatus::Success ||
            !current.entry.has_value()) {
            ADD_FAILURE() << "cursor read failed";
            break;
        }

        entries.push_back(std::move(*current.entry));
        KeyStoreCursorStatus next = cursor.next();
        if (next == KeyStoreCursorStatus::EndOfScan) break;
        if (next != KeyStoreCursorStatus::Success) {
            ADD_FAILURE() << "cursor advance failed";
            break;
        }
    }

    return entries;
}

class KeyStoreIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        const std::string suffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        temp_dir = std::filesystem::temp_directory_path() /
            ("stoneleafdb_keystore_test_" + suffix);
        db_path = temp_dir / "test.db";
        std::filesystem::create_directories(temp_dir);

        log = std::make_unique<Log>(key_store_wal_config());
        log->open((temp_dir / "wal").string());
        transaction_manager = std::make_unique<TransactionManager>(
            *log,
            lock_manager,
            store);
        store.attach_transaction_manager(*transaction_manager);
        ASSERT_EQ(store.open(db_path.string()), KeyStoreStatus::Success);
    }

    void TearDown() override {
        EXPECT_EQ(store.close(), KeyStoreStatus::Success);
        transaction_manager.reset();
        log.reset();

        std::error_code error;
        std::filesystem::remove_all(temp_dir, error);
    }

    TransactionHandle begin() {
        return transaction_manager->begin();
    }

    void commit(const TransactionHandle &transaction) {
        ASSERT_EQ(
            transaction_manager->commit(transaction),
            CommitStatus::Success);
    }

    void put_committed(const KeyInput &key, const ValueInput &value) {
        TransactionHandle transaction = begin();
        ASSERT_EQ(
            store.put(transaction, encode_key(key), encode_value(value)),
            KeyStoreStatus::Success);
        commit(transaction);
    }

    void expect_value(
        const KeyInput &key,
        const ValueInput &expected
    ) {
        TransactionHandle transaction = begin();
        KeyStoreGetResult result = store.get(transaction, encode_key(key));
        ASSERT_EQ(result.status, KeyStoreStatus::Success);
        ASSERT_TRUE(result.value.has_value());
        expect_value_input(*result.value, expected);
        commit(transaction);
    }

    void expect_missing(const KeyInput &key) {
        TransactionHandle transaction = begin();
        KeyStoreGetResult result = store.get(transaction, encode_key(key));
        EXPECT_EQ(result.status, KeyStoreStatus::KeyNotFound);
        EXPECT_FALSE(result.value.has_value());
        commit(transaction);
    }

    std::filesystem::path temp_dir;
    std::filesystem::path db_path;
    Log *unused = nullptr;
    KeyStore store;
    LockManager lock_manager;
    std::unique_ptr<Log> log;
    std::unique_ptr<TransactionManager> transaction_manager;
};

TEST_F(KeyStoreIntegrationTest, RequiresAnActiveTransactionForPointOperations) {
    const Key key = encode_key(KeyInput{std::uint64_t{1}});
    const Value value = encode_value(ValueInput{std::string{"value"}});

    EXPECT_EQ(store.get({}, key).status, KeyStoreStatus::TransactionNotFound);
    EXPECT_EQ(store.put({}, key, value), KeyStoreStatus::TransactionNotFound);
    EXPECT_EQ(store.remove({}, key).status, KeyStoreStatus::TransactionNotFound);

    TransactionHandle ended = begin();
    commit(ended);
    EXPECT_EQ(
        store.get(ended, key).status,
        KeyStoreStatus::TransactionNotFound);
}

TEST_F(KeyStoreIntegrationTest, ValidatesRepresentationsBeforeMutation) {
    TransactionHandle transaction = begin();
    const Key key = encode_key(KeyInput{std::uint64_t{1}});
    const Value value = encode_value(ValueInput{std::string{"value"}});
    const std::string oversized(
        static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()) + 1,
        'x');

    EXPECT_EQ(
        store.put(transaction, KeyCodec::make_string(oversized), value),
        KeyStoreStatus::InvalidKey);
    EXPECT_EQ(
        store.put(transaction, key, ValueCodec::make_char(oversized)),
        KeyStoreStatus::InvalidValue);

    Value malformed_bool{};
    malformed_bool.type = ValueType::Bool;
    malformed_bool.size = 1;
    malformed_bool.data = {'x'};
    EXPECT_EQ(
        store.put(transaction, key, malformed_bool),
        KeyStoreStatus::InvalidValue);
    commit(transaction);
}

TEST_F(KeyStoreIntegrationTest, CommitsInsertUpdateAndRemove) {
    const KeyInput key = std::string{"name"};
    put_committed(key, ValueInput{std::string{"first"}});
    expect_value(key, ValueInput{std::string{"first"}});

    put_committed(key, ValueInput{std::string{"second"}});
    expect_value(key, ValueInput{std::string{"second"}});

    TransactionHandle transaction = begin();
    KeyStoreRemoveResult removed = store.remove(transaction, encode_key(key));
    ASSERT_EQ(removed.status, KeyStoreStatus::Success);
    ASSERT_TRUE(removed.value.has_value());
    expect_value_input(*removed.value, ValueInput{std::string{"second"}});
    commit(transaction);
    expect_missing(key);
}

TEST_F(KeyStoreIntegrationTest, AbortRestoresInsertUpdateAndRemove) {
    const KeyInput inserted = std::uint64_t{1};
    TransactionHandle insert_transaction = begin();
    ASSERT_EQ(
        store.put(
            insert_transaction,
            encode_key(inserted),
            encode_value(ValueInput{std::string{"temporary"}})),
        KeyStoreStatus::Success);
    ASSERT_EQ(
        transaction_manager->abort(
            insert_transaction,
            AbortReason::ClientRequest),
        AbortStatus::Success);
    expect_missing(inserted);

    const KeyInput existing = std::uint64_t{2};
    put_committed(existing, ValueInput{std::string{"original"}});

    TransactionHandle update_transaction = begin();
    ASSERT_EQ(
        store.put(
            update_transaction,
            encode_key(existing),
            encode_value(ValueInput{std::string{"changed"}})),
        KeyStoreStatus::Success);
    ASSERT_EQ(
        transaction_manager->abort(
            update_transaction,
            AbortReason::ClientRequest),
        AbortStatus::Success);
    expect_value(existing, ValueInput{std::string{"original"}});

    TransactionHandle remove_transaction = begin();
    ASSERT_EQ(
        store.remove(remove_transaction, encode_key(existing)).status,
        KeyStoreStatus::Success);
    ASSERT_EQ(
        transaction_manager->abort(
            remove_transaction,
            AbortReason::ClientRequest),
        AbortStatus::Success);
    expect_value(existing, ValueInput{std::string{"original"}});
}

TEST_F(KeyStoreIntegrationTest, WalContainsActionsAndCompensations) {
    TransactionHandle transaction = begin();
    ASSERT_EQ(
        store.put(
            transaction,
            encode_key(KeyInput{std::uint64_t{7}}),
            encode_value(ValueInput{std::string{"value"}})),
        KeyStoreStatus::Success);
    ASSERT_EQ(
        transaction_manager->abort(transaction, AbortReason::ClientRequest),
        AbortStatus::Success);

    bool saw_action = false;
    bool saw_compensation = false;
    for (const WalRecord &record : log->scan()) {
        saw_action = saw_action || record.type == WalRecordType::BTreeAction;
        saw_compensation = saw_compensation ||
            record.type == WalRecordType::Compensation;
    }
    EXPECT_TRUE(saw_action);
    EXPECT_TRUE(saw_compensation);
}

TEST_F(KeyStoreIntegrationTest, ReopenPreservesCommittedValues) {
    put_committed(
        KeyInput{std::string{"persistent"}},
        ValueInput{std::string{"value"}});
    ASSERT_EQ(store.close(), KeyStoreStatus::Success);
    ASSERT_EQ(store.open(db_path.string()), KeyStoreStatus::Success);
    expect_value(
        KeyInput{std::string{"persistent"}},
        ValueInput{std::string{"value"}});
}

TEST_F(KeyStoreIntegrationTest, OrderedScansReadCommittedContents) {
    put_committed(KeyInput{std::string{"aa"}}, ValueInput{std::uint64_t{1}});
    put_committed(KeyInput{std::string{"ab"}}, ValueInput{std::uint64_t{2}});
    put_committed(KeyInput{std::string{"ba"}}, ValueInput{std::uint64_t{3}});

    KeyStoreScanResult prefix = store.scan_prefix(KeyCodec::make_string("a"));
    ASSERT_EQ(prefix.status, KeyStoreStatus::Success);
    ASSERT_TRUE(prefix.cursor.has_value());
    std::vector<KeyStoreEntry> entries = drain_cursor(*prefix.cursor);
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(KeyCodec::decode(entries[0].key), std::optional<KeyInput>{"aa"});
    EXPECT_EQ(KeyCodec::decode(entries[1].key), std::optional<KeyInput>{"ab"});
    EXPECT_EQ(prefix.cursor->close(), KeyStoreCursorStatus::Success);
}

} // namespace

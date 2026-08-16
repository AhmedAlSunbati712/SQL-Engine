#include <gtest/gtest.h>

#include <KeyCodec.h>
#include <KeyStore.h>
#include <LockManager/LockManager.h>
#include <Log/Log.h>
#include <ValueCodec.h>

#include <chrono>
#include <atomic>
#include <barrier>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace {

class KeyStoreNoopUndoExecutor final : public TransactionUndoExecutor {
public:
    void undo(
        Transaction&,
        const UndoDescriptor&,
        CompensationAppender) override {
    }
};

Config key_store_wal_config() {
    return {
        .max_index_bytes = 100 * Index::ENTRY_SIZE,
        .max_store_bytes = 1024 * 1024,
        .initial_lsn = 1,
    };
}

Key encode_key(const KeyInput &input) {
    return KeyCodec::encode(input).value();
}

Value encode_value(const ValueInput &input) {
    return ValueCodec::encode(input).value();
}

void expect_key_input(const Key &key, const KeyInput &expected) {
    std::optional<KeyInput> decoded = KeyCodec::decode(key);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, expected);
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
        if (
            current.status != KeyStoreCursorStatus::Success ||
            !current.entry.has_value()
        ) {
            ADD_FAILURE() << "cursor current failed with status "
                << static_cast<int>(current.status);
            break;
        }

        entries.push_back(std::move(*current.entry));
        KeyStoreCursorStatus next_status = cursor.next();
        if (next_status == KeyStoreCursorStatus::EndOfScan) break;
        if (next_status != KeyStoreCursorStatus::Success) {
            ADD_FAILURE() << "cursor next failed with status "
                << static_cast<int>(next_status);
            break;
        }
    }

    return entries;
}

class KeyStoreIntegrationTest : public ::testing::Test {
    protected:
        void SetUp() override {
            const auto unique_suffix = std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()
            );
            temp_dir = std::filesystem::temp_directory_path() /
                ("stoneleafdb_keystore_test_" + unique_suffix);
            db_path = temp_dir / "test.db";
            std::filesystem::create_directories(temp_dir);
        }

        void TearDown() override {
            std::error_code ec;
            std::filesystem::remove_all(temp_dir, ec);
        }

        void expect_value(
            KeyStore &store,
            const KeyInput &key,
            const ValueInput &expected
        ) {
            KeyStoreGetResult result = store.get(encode_key(key));
            ASSERT_EQ(result.status, KeyStoreStatus::Success);
            ASSERT_TRUE(result.value.has_value());
            expect_value_input(*result.value, expected);
        }

        void expect_missing(KeyStore &store, const KeyInput &key) {
            KeyStoreGetResult result = store.get(encode_key(key));
            EXPECT_EQ(result.status, KeyStoreStatus::KeyNotFound);
            EXPECT_FALSE(result.value.has_value());
        }

        std::filesystem::path temp_dir;
        std::filesystem::path db_path;
};

TEST_F(KeyStoreIntegrationTest, LifecycleAndRepresentationValidation) {
    KeyStore store;
    const Key key = encode_key(KeyInput{std::uint64_t{1}});
    const Value value = encode_value(ValueInput{std::string{"value"}});

    EXPECT_EQ(store.get(key).status, KeyStoreStatus::NotOpen);
    EXPECT_EQ(store.put(key, value), KeyStoreStatus::NotOpen);
    EXPECT_EQ(store.remove(key).status, KeyStoreStatus::NotOpen);
    EXPECT_EQ(store.begin_write_transaction(), KeyStoreStatus::NotOpen);
    EXPECT_EQ(store.commit(), KeyStoreStatus::NotOpen);
    EXPECT_EQ(store.rollback(), KeyStoreStatus::NotOpen);
    EXPECT_EQ(store.scan().status, KeyStoreStatus::NotOpen);
    EXPECT_EQ(store.close(), KeyStoreStatus::Success);

    EXPECT_EQ(
        store.open(temp_dir.string()),
        KeyStoreStatus::FailedToOpen
    );
    ASSERT_EQ(store.open(db_path.string()), KeyStoreStatus::Success);
    EXPECT_EQ(store.open(db_path.string()), KeyStoreStatus::AlreadyOpen);

    const std::string oversized(
        static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()) + 1,
        'x'
    );
    EXPECT_EQ(
        store.put(KeyCodec::make_string(oversized), value),
        KeyStoreStatus::InvalidKey
    );
    EXPECT_EQ(
        store.put(key, ValueCodec::make_char(oversized)),
        KeyStoreStatus::InvalidValue
    );
    EXPECT_EQ(
        store.scan_prefix(KeyCodec::make_string(oversized)).status,
        KeyStoreStatus::InvalidKey
    );
    EXPECT_EQ(
        store.scan_prefix(KeyCodec::make_uint64(1)).status,
        KeyStoreStatus::InvalidKey
    );

    Value malformed_bool{};
    malformed_bool.type = ValueType::Bool;
    malformed_bool.size = 1;
    malformed_bool.data = {'x'};
    EXPECT_EQ(store.put(key, malformed_bool), KeyStoreStatus::InvalidValue);

    EXPECT_EQ(store.close(), KeyStoreStatus::Success);
    EXPECT_EQ(store.close(), KeyStoreStatus::Success);
}

TEST_F(KeyStoreIntegrationTest, TransactionAwareOperationsLockAndAppendWal) {
    Log log(key_store_wal_config());
    log.open((temp_dir / "wal").string());
    LockManager lock_manager;
    KeyStoreNoopUndoExecutor undo_executor;
    TransactionManager transaction_manager(log, lock_manager, undo_executor);

    KeyStore store;
    store.attach_transaction_manager(transaction_manager);
    ASSERT_EQ(store.open(db_path.string()), KeyStoreStatus::Success);

    const TransactionHandle transaction = transaction_manager.begin();
    const Key key = encode_key(KeyInput{std::uint64_t{9}});
    const Value value = encode_value(ValueInput{std::string{"nine"}});

    EXPECT_EQ(
        store.put(transaction, key, value),
        KeyStoreStatus::Success);
    EXPECT_EQ(transaction->last_lsn(), 2U);

    KeyStoreGetResult get_result = store.get(transaction, key);
    ASSERT_EQ(get_result.status, KeyStoreStatus::Success);
    ASSERT_TRUE(get_result.value.has_value());
    expect_value_input(*get_result.value, ValueInput{std::string{"nine"}});

    KeyStoreRemoveResult remove_result = store.remove(transaction, key);
    ASSERT_EQ(remove_result.status, KeyStoreStatus::Success);
    ASSERT_TRUE(remove_result.value.has_value());
    EXPECT_EQ(transaction->last_lsn(), 3U);
    EXPECT_EQ(log.read(2).type, WalRecordType::BTreeAction);
    EXPECT_EQ(log.read(3).type, WalRecordType::BTreeAction);

    EXPECT_EQ(
        transaction_manager.commit(transaction),
        CommitStatus::Success);
}

TEST_F(KeyStoreIntegrationTest, ConcurrentFirstInsertsShareOneInstalledRoot) {
    Log log(Config{
        .max_index_bytes = 1000 * Index::ENTRY_SIZE,
        .max_store_bytes = 16 * 1024 * 1024,
        .initial_lsn = 1,
    });
    log.open((temp_dir / "wal").string());
    LockManager lock_manager;
    KeyStore store;
    TransactionManager transaction_manager(log, lock_manager, store);
    store.attach_transaction_manager(transaction_manager);
    ASSERT_EQ(store.open(db_path.string()), KeyStoreStatus::Success);

    constexpr unsigned THREAD_COUNT = 4;
    constexpr unsigned INSERTS_PER_THREAD = 50;
    std::barrier start(THREAD_COUNT);
    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;

    for (unsigned thread = 0; thread < THREAD_COUNT; ++thread) {
        threads.emplace_back([&, thread] {
            start.arrive_and_wait();
            for (unsigned i = 0; i < INSERTS_PER_THREAD && !failed; ++i) {
                const std::uint64_t number =
                    static_cast<std::uint64_t>(i) * THREAD_COUNT + thread;
                TransactionHandle transaction = transaction_manager.begin();
                if (
                    store.put(
                        transaction,
                        KeyCodec::make_uint64(number),
                        ValueCodec::make_varuint(number)) != KeyStoreStatus::Success ||
                    transaction_manager.commit(transaction) != CommitStatus::Success
                ) {
                    failed = true;
                }
            }
        });
    }

    for (std::thread& thread : threads) thread.join();
    ASSERT_FALSE(failed);

    for (std::uint64_t number = 0; number < THREAD_COUNT * INSERTS_PER_THREAD; ++number) {
        EXPECT_EQ(
            store.get(KeyCodec::make_uint64(number)).status,
            KeyStoreStatus::Success);
    }
}

TEST_F(KeyStoreIntegrationTest, AbortAppendsClrBeforeUndoReleasesPageLatches) {
    KeyStore store;
    Log log(key_store_wal_config());
    log.open((temp_dir / "wal").string());
    LockManager lock_manager;
    TransactionManager transaction_manager(log, lock_manager, store);
    store.attach_transaction_manager(transaction_manager);
    ASSERT_EQ(store.open(db_path.string()), KeyStoreStatus::Success);

    const Key key = encode_key(KeyInput{std::uint64_t{17}});
    const Value value = encode_value(ValueInput{std::string{"temporary"}});
    const TransactionHandle writer = transaction_manager.begin();
    ASSERT_EQ(store.put(writer, key, value), KeyStoreStatus::Success);

    ASSERT_EQ(
        transaction_manager.abort(writer, AbortReason::ClientRequest),
        AbortStatus::Success);
    const std::vector<WalRecord> records = log.scan();
    ASSERT_EQ(records.size(), 5U);
    EXPECT_EQ(records[0].type, WalRecordType::TxnBegin);
    EXPECT_EQ(records[1].type, WalRecordType::BTreeAction);
    EXPECT_EQ(records[2].type, WalRecordType::TxnAbort);
    EXPECT_EQ(records[3].type, WalRecordType::Compensation);
    EXPECT_EQ(records[4].type, WalRecordType::TxnEnd);

    const TransactionHandle reader = transaction_manager.begin();
    EXPECT_EQ(store.get(reader, key).status, KeyStoreStatus::KeyNotFound);
    EXPECT_EQ(
        transaction_manager.abort(reader, AbortReason::ClientRequest),
        AbortStatus::Success);
}

TEST_F(KeyStoreIntegrationTest, TypedValuesRoundTripAndRemoveAcrossReopen) {
    const std::vector<std::pair<KeyInput, ValueInput>> entries = {
        {KeyInput{false}, ValueInput{true}},
        {
            KeyInput{std::numeric_limits<std::uint64_t>::max()},
            ValueInput{std::numeric_limits<std::uint64_t>::max()}
        },
        {
            KeyInput{std::numeric_limits<std::int64_t>::min()},
            ValueInput{std::numeric_limits<std::int64_t>::min()}
        },
        {
            KeyInput{std::string{"key\0text", 8}},
            ValueInput{std::string{"value\0text", 10}}
        },
        {
            KeyInput{std::vector<char>{'b', '\0', 'y'}},
            ValueInput{std::int64_t{-17}}
        }
    };

    {
        KeyStore store;
        ASSERT_EQ(store.open(db_path.string()), KeyStoreStatus::Success);
        for (const auto &[key, value] : entries) {
            ASSERT_EQ(
                store.put(encode_key(key), encode_value(value)),
                KeyStoreStatus::Success
            );
        }
        ASSERT_EQ(store.close(), KeyStoreStatus::Success);
    }

    {
        KeyStore store;
        ASSERT_EQ(store.open(db_path.string()), KeyStoreStatus::Success);
        for (const auto &[key, value] : entries) {
            expect_value(store, key, value);
        }

        const ValueInput replacement = std::string{"replacement"};
        ASSERT_EQ(
            store.put(encode_key(entries[0].first), encode_value(replacement)),
            KeyStoreStatus::Success
        );
        expect_value(store, entries[0].first, replacement);

        KeyStoreRemoveResult removed = store.remove(encode_key(entries[3].first));
        ASSERT_EQ(removed.status, KeyStoreStatus::Success);
        ASSERT_TRUE(removed.value.has_value());
        expect_value_input(*removed.value, entries[3].second);
        expect_missing(store, entries[3].first);
        ASSERT_EQ(store.close(), KeyStoreStatus::Success);
    }

    KeyStore reopened;
    ASSERT_EQ(reopened.open(db_path.string()), KeyStoreStatus::Success);
    expect_value(reopened, entries[0].first, ValueInput{std::string{"replacement"}});
    expect_missing(reopened, entries[3].first);
}

TEST_F(KeyStoreIntegrationTest, ExplicitPolicyCommitsAndRollsBack) {
    KeyStore store({
        .write_policy = KeyStoreWritePolicy::ExplicitTransactionsOnly
    });
    ASSERT_EQ(store.open(db_path.string()), KeyStoreStatus::Success);

    const Key first_key = encode_key(KeyInput{std::uint64_t{10}});
    const Key second_key = encode_key(KeyInput{std::uint64_t{20}});
    EXPECT_EQ(
        store.put(first_key, encode_value(ValueInput{std::string{"first"}})),
        KeyStoreStatus::NoActiveTransaction
    );
    EXPECT_EQ(
        store.remove(first_key).status,
        KeyStoreStatus::NoActiveTransaction
    );
    EXPECT_EQ(store.commit(), KeyStoreStatus::NoActiveTransaction);
    EXPECT_EQ(store.rollback(), KeyStoreStatus::NoActiveTransaction);

    ASSERT_EQ(store.begin_write_transaction(), KeyStoreStatus::Success);
    EXPECT_TRUE(store.write_transaction_active());
    EXPECT_EQ(
        store.begin_write_transaction(),
        KeyStoreStatus::TransactionAlreadyActive
    );
    EXPECT_EQ(store.rollback(), KeyStoreStatus::Success);

    ASSERT_EQ(store.begin_write_transaction(), KeyStoreStatus::Success);
    ASSERT_EQ(
        store.put(first_key, encode_value(ValueInput{std::string{"first"}})),
        KeyStoreStatus::Success
    );
    ASSERT_EQ(
        store.put(second_key, encode_value(ValueInput{std::string{"second"}})),
        KeyStoreStatus::Success
    );
    expect_value(store, KeyInput{std::uint64_t{10}}, ValueInput{std::string{"first"}});
    EXPECT_EQ(store.close(), KeyStoreStatus::WriteTransactionActive);
    ASSERT_EQ(store.commit(), KeyStoreStatus::Success);
    EXPECT_FALSE(store.write_transaction_active());

    ASSERT_EQ(store.begin_write_transaction(), KeyStoreStatus::Success);
    KeyStoreRemoveResult removed = store.remove(first_key);
    ASSERT_EQ(removed.status, KeyStoreStatus::Success);
    ASSERT_TRUE(removed.value.has_value());
    expect_value_input(*removed.value, ValueInput{std::string{"first"}});
    expect_missing(store, KeyInput{std::uint64_t{10}});
    ASSERT_EQ(store.rollback(), KeyStoreStatus::Success);
    expect_value(store, KeyInput{std::uint64_t{10}}, ValueInput{std::string{"first"}});

    ASSERT_EQ(store.close(), KeyStoreStatus::Success);

    KeyStore reopened;
    ASSERT_EQ(reopened.open(db_path.string()), KeyStoreStatus::Success);
    expect_value(reopened, KeyInput{std::uint64_t{10}}, ValueInput{std::string{"first"}});
    expect_value(reopened, KeyInput{std::uint64_t{20}}, ValueInput{std::string{"second"}});
}

TEST_F(KeyStoreIntegrationTest, DestructorRollsBackUnresolvedTransaction) {
    const Key key = encode_key(KeyInput{std::uint64_t{77}});

    {
        KeyStore store;
        ASSERT_EQ(store.open(db_path.string()), KeyStoreStatus::Success);
        ASSERT_EQ(store.begin_write_transaction(), KeyStoreStatus::Success);
        ASSERT_EQ(
            store.put(key, encode_value(ValueInput{std::string{"temporary"}})),
            KeyStoreStatus::Success
        );
        EXPECT_EQ(store.close(), KeyStoreStatus::WriteTransactionActive);
    }

    KeyStore reopened;
    ASSERT_EQ(reopened.open(db_path.string()), KeyStoreStatus::Success);
    expect_missing(reopened, KeyInput{std::uint64_t{77}});
}

TEST_F(KeyStoreIntegrationTest, FullScanUsesTypedOrderAcrossLeaves) {
    KeyStore store;
    ASSERT_EQ(store.open(db_path.string()), KeyStoreStatus::Success);
    ASSERT_EQ(store.begin_write_transaction(), KeyStoreStatus::Success);

    ASSERT_EQ(
        store.put(
            encode_key(KeyInput{false}),
            encode_value(ValueInput{false})
        ),
        KeyStoreStatus::Success
    );
    for (std::uint64_t key = 0; key < 260; key++) {
        ASSERT_EQ(
            store.put(
                encode_key(KeyInput{key}),
                encode_value(ValueInput{std::string{"v"}})
            ),
            KeyStoreStatus::Success
        );
    }
    ASSERT_EQ(
        store.put(
            encode_key(KeyInput{std::int64_t{-1}}),
            encode_value(ValueInput{std::string{"i"}})
        ),
        KeyStoreStatus::Success
    );
    ASSERT_EQ(
        store.put(
            encode_key(KeyInput{std::string{"a"}}),
            encode_value(ValueInput{std::string{"s"}})
        ),
        KeyStoreStatus::Success
    );
    ASSERT_EQ(
        store.put(
            encode_key(KeyInput{std::vector<char>{'a'}}),
            encode_value(ValueInput{std::string{"b"}})
        ),
        KeyStoreStatus::Success
    );
    ASSERT_EQ(store.commit(), KeyStoreStatus::Success);

    KeyStoreScanResult scan = store.scan();
    ASSERT_EQ(scan.status, KeyStoreStatus::Success);
    ASSERT_TRUE(scan.cursor.has_value());
    std::vector<KeyStoreEntry> entries = drain_cursor(*scan.cursor);

    ASSERT_EQ(entries.size(), 264u);
    expect_key_input(entries[0].key, KeyInput{false});
    for (std::uint64_t key = 0; key < 260; key++) {
        expect_key_input(
            entries[static_cast<std::size_t>(key) + 1].key,
            KeyInput{key}
        );
    }
    expect_key_input(entries[261].key, KeyInput{std::int64_t{-1}});
    expect_key_input(entries[262].key, KeyInput{std::string{"a"}});
    expect_key_input(entries[263].key, KeyInput{std::vector<char>{'a'}});
    EXPECT_FALSE(scan.cursor->valid());
    EXPECT_EQ(scan.cursor->next(), KeyStoreCursorStatus::EndOfScan);
    EXPECT_EQ(scan.cursor->close(), KeyStoreCursorStatus::Success);
}

TEST_F(KeyStoreIntegrationTest, LowerBoundAndHalfOpenRangeScans) {
    KeyStore store;
    ASSERT_EQ(store.open(db_path.string()), KeyStoreStatus::Success);
    ASSERT_EQ(store.begin_write_transaction(), KeyStoreStatus::Success);
    for (std::uint64_t key : {10, 20, 30, 40}) {
        ASSERT_EQ(
            store.put(
                encode_key(KeyInput{key}),
                encode_value(ValueInput{key})
            ),
            KeyStoreStatus::Success
        );
    }
    ASSERT_EQ(store.commit(), KeyStoreStatus::Success);

    KeyStoreScanResult from = store.scan_from(
        encode_key(KeyInput{std::uint64_t{25}})
    );
    ASSERT_EQ(from.status, KeyStoreStatus::Success);
    ASSERT_TRUE(from.cursor.has_value());
    std::vector<KeyStoreEntry> from_entries = drain_cursor(*from.cursor);
    ASSERT_EQ(from_entries.size(), 2u);
    expect_key_input(from_entries[0].key, KeyInput{std::uint64_t{30}});
    expect_key_input(from_entries[1].key, KeyInput{std::uint64_t{40}});
    ASSERT_EQ(from.cursor->close(), KeyStoreCursorStatus::Success);

    KeyStoreScanResult range = store.scan_range(
        encode_key(KeyInput{std::uint64_t{20}}),
        encode_key(KeyInput{std::uint64_t{40}})
    );
    ASSERT_EQ(range.status, KeyStoreStatus::Success);
    ASSERT_TRUE(range.cursor.has_value());
    std::vector<KeyStoreEntry> range_entries = drain_cursor(*range.cursor);
    ASSERT_EQ(range_entries.size(), 2u);
    expect_key_input(range_entries[0].key, KeyInput{std::uint64_t{20}});
    expect_key_input(range_entries[1].key, KeyInput{std::uint64_t{30}});
    ASSERT_EQ(range.cursor->close(), KeyStoreCursorStatus::Success);

    KeyStoreScanResult empty = store.scan_range(
        encode_key(KeyInput{std::uint64_t{30}}),
        encode_key(KeyInput{std::uint64_t{30}})
    );
    ASSERT_EQ(empty.status, KeyStoreStatus::Success);
    ASSERT_TRUE(empty.cursor.has_value());
    EXPECT_EQ(
        empty.cursor->current().status,
        KeyStoreCursorStatus::EndOfScan
    );
    ASSERT_EQ(empty.cursor->close(), KeyStoreCursorStatus::Success);

    KeyStoreScanResult reversed = store.scan_range(
        encode_key(KeyInput{std::uint64_t{40}}),
        encode_key(KeyInput{std::uint64_t{20}})
    );
    EXPECT_EQ(reversed.status, KeyStoreStatus::InvalidRange);
    EXPECT_FALSE(reversed.cursor.has_value());

    KeyStoreScanResult past_end = store.scan_from(
        encode_key(KeyInput{std::uint64_t{50}})
    );
    ASSERT_EQ(past_end.status, KeyStoreStatus::Success);
    ASSERT_TRUE(past_end.cursor.has_value());
    EXPECT_EQ(
        past_end.cursor->current().status,
        KeyStoreCursorStatus::EndOfScan
    );
    EXPECT_EQ(past_end.cursor->close(), KeyStoreCursorStatus::Success);
}

TEST_F(KeyStoreIntegrationTest, PrefixScansStayWithinKeyFamily) {
    KeyStore store;
    ASSERT_EQ(store.open(db_path.string()), KeyStoreStatus::Success);
    ASSERT_EQ(store.begin_write_transaction(), KeyStoreStatus::Success);

    const std::vector<KeyInput> keys = {
        KeyInput{std::uint64_t{1}},
        KeyInput{std::string{"app"}},
        KeyInput{std::string{"apple"}},
        KeyInput{std::string{"apricot"}},
        KeyInput{std::string{"banana"}},
        KeyInput{std::vector<char>{'a'}},
        KeyInput{std::vector<char>{'a', '\0'}},
        KeyInput{std::vector<char>{'b'}}
    };
    for (const KeyInput &key : keys) {
        ASSERT_EQ(
            store.put(
                encode_key(key),
                encode_value(ValueInput{std::string{"v"}})
            ),
            KeyStoreStatus::Success
        );
    }
    ASSERT_EQ(store.commit(), KeyStoreStatus::Success);

    KeyStoreScanResult strings = store.scan_prefix(
        encode_key(KeyInput{std::string{"app"}})
    );
    ASSERT_EQ(strings.status, KeyStoreStatus::Success);
    ASSERT_TRUE(strings.cursor.has_value());
    std::vector<KeyStoreEntry> string_entries = drain_cursor(*strings.cursor);
    ASSERT_EQ(string_entries.size(), 2u);
    expect_key_input(string_entries[0].key, KeyInput{std::string{"app"}});
    expect_key_input(string_entries[1].key, KeyInput{std::string{"apple"}});
    ASSERT_EQ(strings.cursor->close(), KeyStoreCursorStatus::Success);

    KeyStoreScanResult bytes = store.scan_prefix(
        encode_key(KeyInput{std::vector<char>{'a'}})
    );
    ASSERT_EQ(bytes.status, KeyStoreStatus::Success);
    ASSERT_TRUE(bytes.cursor.has_value());
    std::vector<KeyStoreEntry> byte_entries = drain_cursor(*bytes.cursor);
    ASSERT_EQ(byte_entries.size(), 2u);
    expect_key_input(byte_entries[0].key, KeyInput{std::vector<char>{'a'}});
    expect_key_input(
        byte_entries[1].key,
        KeyInput{std::vector<char>{'a', '\0'}}
    );
    ASSERT_EQ(bytes.cursor->close(), KeyStoreCursorStatus::Success);

    KeyStoreScanResult all_strings = store.scan_prefix(
        encode_key(KeyInput{std::string{}})
    );
    ASSERT_EQ(all_strings.status, KeyStoreStatus::Success);
    ASSERT_TRUE(all_strings.cursor.has_value());
    std::vector<KeyStoreEntry> all_string_entries =
        drain_cursor(*all_strings.cursor);
    ASSERT_EQ(all_string_entries.size(), 4u);
    for (const KeyStoreEntry &entry : all_string_entries) {
        EXPECT_EQ(entry.key.type, KeyType::String);
    }
    EXPECT_EQ(all_strings.cursor->close(), KeyStoreCursorStatus::Success);
}

TEST_F(KeyStoreIntegrationTest, CursorOwnershipBlocksWritesAndBoundaries) {
    KeyStore store;
    ASSERT_EQ(store.open(db_path.string()), KeyStoreStatus::Success);
    ASSERT_EQ(
        store.put(
            encode_key(KeyInput{std::uint64_t{1}}),
            encode_value(ValueInput{std::string{"one"}})
        ),
        KeyStoreStatus::Success
    );

    KeyStoreScanResult scan = store.scan();
    ASSERT_EQ(scan.status, KeyStoreStatus::Success);
    ASSERT_TRUE(scan.cursor.has_value());

    KeyStoreCursor moved(std::move(*scan.cursor));
    EXPECT_EQ(
        scan.cursor->current().status,
        KeyStoreCursorStatus::Closed
    );
    EXPECT_TRUE(moved.valid());
    EXPECT_EQ(
        store.put(
            encode_key(KeyInput{std::uint64_t{2}}),
            encode_value(ValueInput{std::string{"two"}})
        ),
        KeyStoreStatus::CursorActive
    );
    EXPECT_EQ(store.close(), KeyStoreStatus::CursorActive);

    ASSERT_EQ(store.begin_write_transaction(), KeyStoreStatus::Success);
    EXPECT_EQ(
        store.put(
            encode_key(KeyInput{std::uint64_t{2}}),
            encode_value(ValueInput{std::string{"two"}})
        ),
        KeyStoreStatus::CursorActive
    );
    EXPECT_EQ(store.commit(), KeyStoreStatus::CursorActive);
    EXPECT_TRUE(store.write_transaction_active());

    EXPECT_EQ(moved.close(), KeyStoreCursorStatus::Success);
    EXPECT_EQ(moved.close(), KeyStoreCursorStatus::Success);
    EXPECT_EQ(moved.current().status, KeyStoreCursorStatus::Closed);

    ASSERT_EQ(
        store.put(
            encode_key(KeyInput{std::uint64_t{2}}),
            encode_value(ValueInput{std::string{"two"}})
        ),
        KeyStoreStatus::Success
    );
    ASSERT_EQ(store.commit(), KeyStoreStatus::Success);
    expect_value(
        store,
        KeyInput{std::uint64_t{2}},
        ValueInput{std::string{"two"}}
    );
}

TEST_F(KeyStoreIntegrationTest, ScanCanReadUncommittedWritesBeforeCommit) {
    KeyStore store;
    ASSERT_EQ(store.open(db_path.string()), KeyStoreStatus::Success);
    ASSERT_EQ(store.begin_write_transaction(), KeyStoreStatus::Success);
    ASSERT_EQ(
        store.put(
            encode_key(KeyInput{std::uint64_t{9}}),
            encode_value(ValueInput{std::string{"nine"}})
        ),
        KeyStoreStatus::Success
    );

    KeyStoreScanResult scan = store.scan();
    ASSERT_EQ(scan.status, KeyStoreStatus::Success);
    ASSERT_TRUE(scan.cursor.has_value());
    KeyStoreCursorResult current = scan.cursor->current();
    ASSERT_EQ(current.status, KeyStoreCursorStatus::Success);
    ASSERT_TRUE(current.entry.has_value());
    expect_key_input(current.entry->key, KeyInput{std::uint64_t{9}});
    expect_value_input(current.entry->value, ValueInput{std::string{"nine"}});
    EXPECT_EQ(store.commit(), KeyStoreStatus::CursorActive);
    ASSERT_EQ(scan.cursor->close(), KeyStoreCursorStatus::Success);
    ASSERT_EQ(store.commit(), KeyStoreStatus::Success);
}

TEST_F(KeyStoreIntegrationTest, WriterConflictRequiresExplicitRollback) {
    {
        KeyStore initializer;
        ASSERT_EQ(
            initializer.open(db_path.string()),
            KeyStoreStatus::Success
        );
        ASSERT_EQ(
            initializer.put(
                encode_key(KeyInput{std::uint64_t{1}}),
                encode_value(ValueInput{std::string{"seed"}})
            ),
            KeyStoreStatus::Success
        );
        ASSERT_EQ(initializer.close(), KeyStoreStatus::Success);
    }

    int ready_pipe[2];
    int release_pipe[2];
    ASSERT_EQ(::pipe(ready_pipe), 0);
    ASSERT_EQ(::pipe(release_pipe), 0);

    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        ::close(ready_pipe[0]);
        ::close(release_pipe[1]);

        KeyStore holder({
            .write_policy = KeyStoreWritePolicy::ExplicitTransactionsOnly
        });
        bool ok =
            holder.open(db_path.string()) == KeyStoreStatus::Success &&
            holder.begin_write_transaction() == KeyStoreStatus::Success &&
            holder.put(
                encode_key(KeyInput{std::uint64_t{1}}),
                encode_value(ValueInput{std::string{"held"}})
            ) == KeyStoreStatus::Success;

        const char ready = ok ? '1' : '0';
        if (::write(ready_pipe[1], &ready, 1) != 1) _exit(2);

        char release = '\0';
        if (::read(release_pipe[0], &release, 1) != 1) _exit(3);
        if (ok && holder.rollback() != KeyStoreStatus::Success) _exit(4);
        if (holder.close() != KeyStoreStatus::Success) _exit(5);
        _exit(ok ? 0 : 6);
    }

    ::close(ready_pipe[1]);
    ::close(release_pipe[0]);

    char ready = '\0';
    ASSERT_EQ(::read(ready_pipe[0], &ready, 1), 1);
    ASSERT_EQ(ready, '1');

    KeyStore contender({
        .write_policy = KeyStoreWritePolicy::ExplicitTransactionsOnly
    });
    EXPECT_EQ(contender.open(db_path.string()), KeyStoreStatus::Success);
    EXPECT_EQ(
        contender.begin_write_transaction(),
        KeyStoreStatus::Success
    );
    EXPECT_EQ(
        contender.put(
            encode_key(KeyInput{std::uint64_t{2}}),
            encode_value(ValueInput{std::string{"blocked"}})
        ),
        KeyStoreStatus::WriteFailed
    );
    EXPECT_TRUE(contender.write_transaction_active());
    EXPECT_EQ(
        contender.put(
            encode_key(KeyInput{std::uint64_t{3}}),
            encode_value(ValueInput{std::string{"retry"}})
        ),
        KeyStoreStatus::TransactionNeedsRollback
    );
    EXPECT_EQ(
        contender.close(),
        KeyStoreStatus::TransactionNeedsRollback
    );
    EXPECT_EQ(contender.rollback(), KeyStoreStatus::Success);
    EXPECT_EQ(contender.close(), KeyStoreStatus::Success);

    const char release = '1';
    EXPECT_EQ(::write(release_pipe[1], &release, 1), 1);

    int child_status = 0;
    EXPECT_EQ(::waitpid(child, &child_status, 0), child);
    EXPECT_TRUE(WIFEXITED(child_status));
    EXPECT_EQ(WEXITSTATUS(child_status), 0);

    ::close(ready_pipe[0]);
    ::close(release_pipe[1]);
}

TEST_F(KeyStoreIntegrationTest, AutocommitFailureRollsBackBlockedCommit) {
    {
        KeyStore initializer;
        ASSERT_EQ(
            initializer.open(db_path.string()),
            KeyStoreStatus::Success
        );
        ASSERT_EQ(
            initializer.put(
                encode_key(KeyInput{std::uint64_t{1}}),
                encode_value(ValueInput{std::string{"seed"}})
            ),
            KeyStoreStatus::Success
        );
        ASSERT_EQ(initializer.close(), KeyStoreStatus::Success);
    }

    int ready_pipe[2];
    int release_pipe[2];
    ASSERT_EQ(::pipe(ready_pipe), 0);
    ASSERT_EQ(::pipe(release_pipe), 0);

    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        ::close(ready_pipe[0]);
        ::close(release_pipe[1]);

        KeyStore reader;
        bool ok = reader.open(db_path.string()) == KeyStoreStatus::Success;
        KeyStoreScanResult scan;
        if (ok) {
            scan = reader.scan();
            ok = scan.status == KeyStoreStatus::Success &&
                scan.cursor.has_value();
        }

        const char ready = ok ? '1' : '0';
        if (::write(ready_pipe[1], &ready, 1) != 1) _exit(2);

        char release = '\0';
        if (::read(release_pipe[0], &release, 1) != 1) _exit(3);
        if (
            ok &&
            scan.cursor->close() != KeyStoreCursorStatus::Success
        ) {
            _exit(4);
        }
        if (reader.close() != KeyStoreStatus::Success) _exit(5);
        _exit(ok ? 0 : 6);
    }

    ::close(ready_pipe[1]);
    ::close(release_pipe[0]);

    char ready = '\0';
    ASSERT_EQ(::read(ready_pipe[0], &ready, 1), 1);
    ASSERT_EQ(ready, '1');

    KeyStore writer;
    EXPECT_EQ(writer.open(db_path.string()), KeyStoreStatus::Success);
    EXPECT_EQ(
        writer.put(
            encode_key(KeyInput{std::uint64_t{5}}),
            encode_value(ValueInput{std::string{"not committed"}})
        ),
        KeyStoreStatus::CommitFailed
    );
    expect_missing(writer, KeyInput{std::uint64_t{5}});

    const char release = '1';
    EXPECT_EQ(::write(release_pipe[1], &release, 1), 1);

    int child_status = 0;
    EXPECT_EQ(::waitpid(child, &child_status, 0), child);
    EXPECT_TRUE(WIFEXITED(child_status));
    EXPECT_EQ(WEXITSTATUS(child_status), 0);
    EXPECT_EQ(writer.close(), KeyStoreStatus::Success);

    ::close(ready_pipe[0]);
    ::close(release_pipe[1]);

    KeyStore reopened;
    ASSERT_EQ(reopened.open(db_path.string()), KeyStoreStatus::Success);
    expect_missing(reopened, KeyInput{std::uint64_t{5}});
}

} // namespace

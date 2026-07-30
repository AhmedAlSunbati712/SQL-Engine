#include <gtest/gtest.h>

#include <KeyStore.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace {

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
            KeyStoreGetResult result = store.get(key);
            ASSERT_EQ(result.status, KeyStoreStatus::Success);
            ASSERT_TRUE(result.value.has_value());
            EXPECT_EQ(*result.value, expected);
        }

        void expect_missing(KeyStore &store, const KeyInput &key) {
            KeyStoreGetResult result = store.get(key);
            EXPECT_EQ(result.status, KeyStoreStatus::KeyNotFound);
            EXPECT_FALSE(result.value.has_value());
        }

        std::filesystem::path temp_dir;
        std::filesystem::path db_path;
};

TEST_F(KeyStoreIntegrationTest, LifecycleAndRepresentationValidation) {
    KeyStore store;
    const KeyInput key = std::uint64_t{1};
    const ValueInput value = std::string{"value"};

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
        store.put(KeyInput{oversized}, value),
        KeyStoreStatus::InvalidKey
    );
    EXPECT_EQ(
        store.put(key, ValueInput{oversized}),
        KeyStoreStatus::InvalidValue
    );
    EXPECT_EQ(
        store.scan_prefix(KeyPrefix{oversized}).status,
        KeyStoreStatus::InvalidKey
    );

    EXPECT_EQ(store.close(), KeyStoreStatus::Success);
    EXPECT_EQ(store.close(), KeyStoreStatus::Success);
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
            ASSERT_EQ(store.put(key, value), KeyStoreStatus::Success);
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
            store.put(entries[0].first, replacement),
            KeyStoreStatus::Success
        );
        expect_value(store, entries[0].first, replacement);

        KeyStoreRemoveResult removed = store.remove(entries[3].first);
        ASSERT_EQ(removed.status, KeyStoreStatus::Success);
        ASSERT_TRUE(removed.value.has_value());
        EXPECT_EQ(*removed.value, entries[3].second);
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

    const KeyInput first_key = std::uint64_t{10};
    const KeyInput second_key = std::uint64_t{20};
    EXPECT_EQ(
        store.put(first_key, ValueInput{std::string{"first"}}),
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
        store.put(first_key, ValueInput{std::string{"first"}}),
        KeyStoreStatus::Success
    );
    ASSERT_EQ(
        store.put(second_key, ValueInput{std::string{"second"}}),
        KeyStoreStatus::Success
    );
    expect_value(store, first_key, ValueInput{std::string{"first"}});
    EXPECT_EQ(store.close(), KeyStoreStatus::WriteTransactionActive);
    ASSERT_EQ(store.commit(), KeyStoreStatus::Success);
    EXPECT_FALSE(store.write_transaction_active());

    ASSERT_EQ(store.begin_write_transaction(), KeyStoreStatus::Success);
    KeyStoreRemoveResult removed = store.remove(first_key);
    ASSERT_EQ(removed.status, KeyStoreStatus::Success);
    ASSERT_TRUE(removed.value.has_value());
    EXPECT_EQ(*removed.value, ValueInput{std::string{"first"}});
    expect_missing(store, first_key);
    ASSERT_EQ(store.rollback(), KeyStoreStatus::Success);
    expect_value(store, first_key, ValueInput{std::string{"first"}});

    ASSERT_EQ(store.close(), KeyStoreStatus::Success);

    KeyStore reopened;
    ASSERT_EQ(reopened.open(db_path.string()), KeyStoreStatus::Success);
    expect_value(reopened, first_key, ValueInput{std::string{"first"}});
    expect_value(reopened, second_key, ValueInput{std::string{"second"}});
}

TEST_F(KeyStoreIntegrationTest, DestructorRollsBackUnresolvedTransaction) {
    const KeyInput key = std::uint64_t{77};

    {
        KeyStore store;
        ASSERT_EQ(store.open(db_path.string()), KeyStoreStatus::Success);
        ASSERT_EQ(store.begin_write_transaction(), KeyStoreStatus::Success);
        ASSERT_EQ(
            store.put(key, ValueInput{std::string{"temporary"}}),
            KeyStoreStatus::Success
        );
        EXPECT_EQ(store.close(), KeyStoreStatus::WriteTransactionActive);
    }

    KeyStore reopened;
    ASSERT_EQ(reopened.open(db_path.string()), KeyStoreStatus::Success);
    expect_missing(reopened, key);
}

TEST_F(KeyStoreIntegrationTest, FullScanUsesTypedOrderAcrossLeaves) {
    KeyStore store;
    ASSERT_EQ(store.open(db_path.string()), KeyStoreStatus::Success);
    ASSERT_EQ(store.begin_write_transaction(), KeyStoreStatus::Success);

    ASSERT_EQ(store.put(KeyInput{false}, ValueInput{false}), KeyStoreStatus::Success);
    for (std::uint64_t key = 0; key < 260; key++) {
        ASSERT_EQ(
            store.put(KeyInput{key}, ValueInput{std::string{"v"}}),
            KeyStoreStatus::Success
        );
    }
    ASSERT_EQ(
        store.put(KeyInput{std::int64_t{-1}}, ValueInput{std::string{"i"}}),
        KeyStoreStatus::Success
    );
    ASSERT_EQ(
        store.put(KeyInput{std::string{"a"}}, ValueInput{std::string{"s"}}),
        KeyStoreStatus::Success
    );
    ASSERT_EQ(
        store.put(
            KeyInput{std::vector<char>{'a'}},
            ValueInput{std::string{"b"}}
        ),
        KeyStoreStatus::Success
    );
    ASSERT_EQ(store.commit(), KeyStoreStatus::Success);

    KeyStoreScanResult scan = store.scan();
    ASSERT_EQ(scan.status, KeyStoreStatus::Success);
    ASSERT_TRUE(scan.cursor.has_value());
    std::vector<KeyStoreEntry> entries = drain_cursor(*scan.cursor);

    ASSERT_EQ(entries.size(), 264u);
    EXPECT_EQ(entries[0].key, KeyInput{false});
    for (std::uint64_t key = 0; key < 260; key++) {
        EXPECT_EQ(entries[static_cast<std::size_t>(key) + 1].key, KeyInput{key});
    }
    EXPECT_EQ(entries[261].key, KeyInput{std::int64_t{-1}});
    EXPECT_EQ(entries[262].key, KeyInput{std::string{"a"}});
    EXPECT_EQ(entries[263].key, KeyInput{std::vector<char>{'a'}});
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
            store.put(KeyInput{key}, ValueInput{key}),
            KeyStoreStatus::Success
        );
    }
    ASSERT_EQ(store.commit(), KeyStoreStatus::Success);

    KeyStoreScanResult from = store.scan_from(KeyInput{std::uint64_t{25}});
    ASSERT_EQ(from.status, KeyStoreStatus::Success);
    ASSERT_TRUE(from.cursor.has_value());
    std::vector<KeyStoreEntry> from_entries = drain_cursor(*from.cursor);
    ASSERT_EQ(from_entries.size(), 2u);
    EXPECT_EQ(from_entries[0].key, KeyInput{std::uint64_t{30}});
    EXPECT_EQ(from_entries[1].key, KeyInput{std::uint64_t{40}});
    ASSERT_EQ(from.cursor->close(), KeyStoreCursorStatus::Success);

    KeyStoreScanResult range = store.scan_range(
        KeyInput{std::uint64_t{20}},
        KeyInput{std::uint64_t{40}}
    );
    ASSERT_EQ(range.status, KeyStoreStatus::Success);
    ASSERT_TRUE(range.cursor.has_value());
    std::vector<KeyStoreEntry> range_entries = drain_cursor(*range.cursor);
    ASSERT_EQ(range_entries.size(), 2u);
    EXPECT_EQ(range_entries[0].key, KeyInput{std::uint64_t{20}});
    EXPECT_EQ(range_entries[1].key, KeyInput{std::uint64_t{30}});
    ASSERT_EQ(range.cursor->close(), KeyStoreCursorStatus::Success);

    KeyStoreScanResult empty = store.scan_range(
        KeyInput{std::uint64_t{30}},
        KeyInput{std::uint64_t{30}}
    );
    ASSERT_EQ(empty.status, KeyStoreStatus::Success);
    ASSERT_TRUE(empty.cursor.has_value());
    EXPECT_EQ(
        empty.cursor->current().status,
        KeyStoreCursorStatus::EndOfScan
    );
    ASSERT_EQ(empty.cursor->close(), KeyStoreCursorStatus::Success);

    KeyStoreScanResult reversed = store.scan_range(
        KeyInput{std::uint64_t{40}},
        KeyInput{std::uint64_t{20}}
    );
    EXPECT_EQ(reversed.status, KeyStoreStatus::InvalidRange);
    EXPECT_FALSE(reversed.cursor.has_value());

    KeyStoreScanResult past_end = store.scan_from(KeyInput{std::uint64_t{50}});
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
            store.put(key, ValueInput{std::string{"v"}}),
            KeyStoreStatus::Success
        );
    }
    ASSERT_EQ(store.commit(), KeyStoreStatus::Success);

    KeyStoreScanResult strings = store.scan_prefix(KeyPrefix{std::string{"app"}});
    ASSERT_EQ(strings.status, KeyStoreStatus::Success);
    ASSERT_TRUE(strings.cursor.has_value());
    std::vector<KeyStoreEntry> string_entries = drain_cursor(*strings.cursor);
    ASSERT_EQ(string_entries.size(), 2u);
    EXPECT_EQ(string_entries[0].key, KeyInput{std::string{"app"}});
    EXPECT_EQ(string_entries[1].key, KeyInput{std::string{"apple"}});
    ASSERT_EQ(strings.cursor->close(), KeyStoreCursorStatus::Success);

    KeyStoreScanResult bytes = store.scan_prefix(
        KeyPrefix{std::vector<char>{'a'}}
    );
    ASSERT_EQ(bytes.status, KeyStoreStatus::Success);
    ASSERT_TRUE(bytes.cursor.has_value());
    std::vector<KeyStoreEntry> byte_entries = drain_cursor(*bytes.cursor);
    ASSERT_EQ(byte_entries.size(), 2u);
    EXPECT_EQ(byte_entries[0].key, KeyInput{std::vector<char>{'a'}});
    EXPECT_EQ(
        byte_entries[1].key,
        (KeyInput{std::vector<char>{'a', '\0'}})
    );
    ASSERT_EQ(bytes.cursor->close(), KeyStoreCursorStatus::Success);

    KeyStoreScanResult all_strings = store.scan_prefix(
        KeyPrefix{std::string{}}
    );
    ASSERT_EQ(all_strings.status, KeyStoreStatus::Success);
    ASSERT_TRUE(all_strings.cursor.has_value());
    std::vector<KeyStoreEntry> all_string_entries =
        drain_cursor(*all_strings.cursor);
    ASSERT_EQ(all_string_entries.size(), 4u);
    for (const KeyStoreEntry &entry : all_string_entries) {
        EXPECT_TRUE(std::holds_alternative<std::string>(entry.key));
    }
    EXPECT_EQ(all_strings.cursor->close(), KeyStoreCursorStatus::Success);
}

TEST_F(KeyStoreIntegrationTest, CursorOwnershipBlocksWritesAndBoundaries) {
    KeyStore store;
    ASSERT_EQ(store.open(db_path.string()), KeyStoreStatus::Success);
    ASSERT_EQ(
        store.put(KeyInput{std::uint64_t{1}}, ValueInput{std::string{"one"}}),
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
            KeyInput{std::uint64_t{2}},
            ValueInput{std::string{"two"}}
        ),
        KeyStoreStatus::CursorActive
    );
    EXPECT_EQ(store.close(), KeyStoreStatus::CursorActive);

    ASSERT_EQ(store.begin_write_transaction(), KeyStoreStatus::Success);
    EXPECT_EQ(
        store.put(
            KeyInput{std::uint64_t{2}},
            ValueInput{std::string{"two"}}
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
            KeyInput{std::uint64_t{2}},
            ValueInput{std::string{"two"}}
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
        store.put(KeyInput{std::uint64_t{9}}, ValueInput{std::string{"nine"}}),
        KeyStoreStatus::Success
    );

    KeyStoreScanResult scan = store.scan();
    ASSERT_EQ(scan.status, KeyStoreStatus::Success);
    ASSERT_TRUE(scan.cursor.has_value());
    KeyStoreCursorResult current = scan.cursor->current();
    ASSERT_EQ(current.status, KeyStoreCursorStatus::Success);
    ASSERT_TRUE(current.entry.has_value());
    EXPECT_EQ(current.entry->key, KeyInput{std::uint64_t{9}});
    EXPECT_EQ(current.entry->value, ValueInput{std::string{"nine"}});
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
                KeyInput{std::uint64_t{1}},
                ValueInput{std::string{"seed"}}
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
                KeyInput{std::uint64_t{1}},
                ValueInput{std::string{"held"}}
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
            KeyInput{std::uint64_t{2}},
            ValueInput{std::string{"blocked"}}
        ),
        KeyStoreStatus::WriteFailed
    );
    EXPECT_TRUE(contender.write_transaction_active());
    EXPECT_EQ(
        contender.put(
            KeyInput{std::uint64_t{3}},
            ValueInput{std::string{"retry"}}
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
                KeyInput{std::uint64_t{1}},
                ValueInput{std::string{"seed"}}
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
            KeyInput{std::uint64_t{5}},
            ValueInput{std::string{"not committed"}}
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

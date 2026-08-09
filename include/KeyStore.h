#pragma once

#include <BTree.h>
#include <BTreeCursor.h>
#include <Key.h>
#include <Value.h>

#include <cstdint>
#include <optional>
#include <string>

enum class KeyStoreWritePolicy : std::uint8_t {
    AutoCommit = 0,
    ExplicitTransactionsOnly
};

struct KeyStoreOptions {
    KeyStoreWritePolicy write_policy = KeyStoreWritePolicy::AutoCommit;
};

enum class KeyStoreStatus : std::uint8_t {
    Success = 0,
    FailedToOpen,
    FailedToClose,
    KeyNotFound,
    InvalidKey,
    InvalidValue,
    InvalidRange,
    NoActiveTransaction,
    TransactionAlreadyActive,
    TransactionNeedsRollback,
    WriteTransactionActive,
    CursorActive,
    WriteFailed,
    CommitFailed,
    RollbackFailed,
    ScanFailed,
    // Appended to preserve the numeric values of the original public statuses.
    NotOpen,
    AlreadyOpen,
    ReadFailed,
    DecodeFailed
};

enum class KeyStoreCursorStatus : std::uint8_t {
    Success = 0,
    EndOfScan,
    NotPositioned,
    Closed,
    DecodeFailed,
    StorageError
};

struct KeyStoreGetResult {
    KeyStoreStatus status = KeyStoreStatus::Success;
    std::optional<Value> value;
};

struct KeyStoreRemoveResult {
    KeyStoreStatus status = KeyStoreStatus::Success;
    std::optional<Value> value;
};

struct KeyStoreEntry {
    Key key;
    Value value;
};

struct KeyStoreCursorResult {
    KeyStoreCursorStatus status = KeyStoreCursorStatus::NotPositioned;
    std::optional<KeyStoreEntry> entry;
};

class KeyStore;

class KeyStoreCursor {
    public:
        ~KeyStoreCursor() = default;

        KeyStoreCursor(const KeyStoreCursor &) = delete;
        KeyStoreCursor &operator=(const KeyStoreCursor &) = delete;
        KeyStoreCursor(KeyStoreCursor &&) noexcept = default;
        KeyStoreCursor &operator=(KeyStoreCursor &&) noexcept = default;

        KeyStoreCursorResult current() const;
        KeyStoreCursorStatus next();
        bool valid() const;
        KeyStoreCursorStatus close();

    private:
        friend class KeyStore;

        explicit KeyStoreCursor(
            BTreeCursor cursor,
            std::optional<Key> exclusive_end = std::nullopt,
            std::optional<Key> required_prefix = std::nullopt
        );

        bool current_key_is_in_bounds(const Key &key) const;
        static KeyStoreCursorStatus translate_cursor_status(BTreeCursorStatus status);

        BTreeCursor cursor;
        std::optional<Key> exclusive_end;
        std::optional<Key> required_prefix;
        bool exhausted_by_bound = false;
};

struct KeyStoreScanResult {
    KeyStoreStatus status = KeyStoreStatus::Success;
    std::optional<KeyStoreCursor> cursor;
};

class KeyStore {
    public:
        explicit KeyStore(KeyStoreOptions options = {});
        ~KeyStore();

        KeyStore(const KeyStore &) = delete;
        KeyStore &operator=(const KeyStore &) = delete;
        KeyStore(KeyStore &&) = delete;
        KeyStore &operator=(KeyStore &&) = delete;

        // A KeyStore owns one open BTree at a time. Any cursor returned from a
        // scan must be destroyed before its KeyStore.
        KeyStoreStatus open(const std::string &db_file);
        // Closing an already-closed store succeeds. Active or failed write
        // transactions must be committed or rolled back before an explicit close.
        KeyStoreStatus close();

        KeyStoreGetResult get(const Key &key);
        KeyStoreStatus put(const Key &key, const Value &value);
        KeyStoreRemoveResult remove(const Key &key);

        KeyStoreStatus begin_write_transaction();
        KeyStoreStatus commit();
        KeyStoreStatus rollback();
        bool write_transaction_active() const;

        // Successful scans return an already-positioned cursor. An empty result
        // is represented by a cursor whose current status is EndOfScan.
        // Full ordered scan, starting at the leftmost leaf.
        KeyStoreScanResult scan();

        // Lower-bound scan over [start, end of tree).
        KeyStoreScanResult scan_from(const Key &start);

        // Half-open range scan over [start, end).
        KeyStoreScanResult scan_range(const Key &start, const Key &end);

        // Scan encoded string or byte keys beginning with prefix.
        KeyStoreScanResult scan_prefix(const Key &prefix);

    private:
        enum class TransactionState : std::uint8_t {
            None = 0,
            Active,
            Failed
        };

        KeyStoreStatus finish_autocommit_write(BTreeStatus write_status);
        KeyStoreStatus handle_transactional_write(BTreeStatus write_status);
        void clear_transaction_state();

        BTree tree;
        KeyStoreOptions options;
        TransactionState transaction_state = TransactionState::None;
        bool transaction_has_writes = false;
        bool is_open = false;
};

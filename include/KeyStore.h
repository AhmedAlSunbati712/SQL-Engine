#pragma once

#include <BTree.h>
#include <BTreeCursor.h>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

using KeyInput = std::variant<
    bool,
    std::uint64_t,
    std::int64_t,
    std::string,
    std::vector<char>
>;

using ValueInput = std::variant<
    bool,
    std::uint64_t,
    std::int64_t,
    std::string
>;

// Prefix scans only apply to the two variable-length key families.
using KeyPrefix = std::variant<std::string, std::vector<char>>;

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
    ScanFailed
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
    std::optional<ValueInput> value;
};

struct KeyStoreRemoveResult {
    KeyStoreStatus status = KeyStoreStatus::Success;
    std::optional<ValueInput> value;
};

struct KeyStoreEntry {
    KeyInput key;
    ValueInput value;
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
        static std::optional<KeyInput> decode_key(const Key &key);
        static std::optional<ValueInput> decode_value(const Value &value);

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

        KeyStoreStatus open(const std::string &db_file);
        KeyStoreStatus close();

        KeyStoreGetResult get(const KeyInput &key);
        KeyStoreStatus put(const KeyInput &key, const ValueInput &value);
        KeyStoreRemoveResult remove(const KeyInput &key);

        KeyStoreStatus begin_write_transaction();
        KeyStoreStatus commit();
        KeyStoreStatus rollback();
        bool write_transaction_active() const;

        // Full ordered scan, starting at the leftmost leaf.
        KeyStoreScanResult scan();

        // Lower-bound scan over [start, end of tree).
        KeyStoreScanResult scan_from(const KeyInput &start);

        // Half-open range scan over [start, end).
        KeyStoreScanResult scan_range(const KeyInput &start, const KeyInput &end);

        // Scan string or byte keys beginning with prefix.
        KeyStoreScanResult scan_prefix(const KeyPrefix &prefix);

    private:
        enum class TransactionState : std::uint8_t {
            None = 0,
            Active,
            Failed
        };

        static std::optional<Key> encode_key(const KeyInput &key);
        static std::optional<Value> encode_value(const ValueInput &value);
        static std::optional<ValueInput> decode_value(const Value &value);

        KeyStoreStatus finish_autocommit_write(BTreeStatus write_status);
        KeyStoreStatus handle_transactional_write(BTreeStatus write_status);
        void clear_transaction_state();

        BTree tree;
        KeyStoreOptions options;
        TransactionState transaction_state = TransactionState::None;
        bool transaction_has_writes = false;
        bool is_open = false;
};

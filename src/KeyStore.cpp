#include <KeyStore.h>

#include <Endian.h>
#include <KeyCodec.h>
#include <ValueCodec.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

/**
 * KeyStore is the client-facing layer above BTree.
 *
 * The BTree deliberately works with encoded Key and Value objects and exposes
 * storage-oriented status values. KeyStore is responsible for the opposite
 * side of that boundary:
 *
 * - convert client variants into the ordered on-disk encodings;
 * - translate BTree and BTreeCursor failures into public KeyStore statuses;
 * - decide whether a successful write is committed immediately or belongs to
 *   an explicit transaction;
 * - keep a failed explicit transaction unusable until rollback; and
 * - attach range and prefix bounds to otherwise unbounded BTree cursors.
 *
 * KeyStore does not add another cache or transaction mechanism. Durability,
 * rollback journaling, page ownership, and cross-process locks remain owned by
 * the BTree/Pager layers below it.
 */

namespace {

// Keys and values use 2-byte payload lengths in the current leaf-page format.
// This guard only protects that representation. It does not solve aggregate
// page-capacity or overflow-page handling.
constexpr std::size_t MAX_ENCODED_PAYLOAD_SIZE =
    std::numeric_limits<std::uint16_t>::max();

std::optional<KeyInput> decode_key_input(const Key &key) {
    /**
     * Convert an encoded storage key back into the matching public variant.
     *
     * Numeric key encodings preserve sort order, so decoding is not always a
     * direct byte copy. UInt64 reverses the big-endian encoding. Int64 also
     * removes the sign-bit bias that makes lexicographic byte order match
     * signed numeric order.
     */
    if (!keycodec::validate_key(key)) return std::nullopt;

    switch (key.type) {
        case KeyType::Bool:
            return KeyInput{key.data[0] == '\1'};
        case KeyType::UInt64:
            return KeyInput{get_u64_be(key.data.data())};
        case KeyType::Int64: {
            // KeyCodec flips the sign bit before storing signed integers. Flip
            // it back, then preserve the resulting bit pattern as int64_t.
            const std::uint64_t encoded = get_u64_be(key.data.data());
            const std::uint64_t raw = encoded ^ (1ULL << 63);
            return KeyInput{std::bit_cast<std::int64_t>(raw)};
        }
        case KeyType::String:
            return KeyInput{std::string(key.data.begin(), key.data.end())};
        case KeyType::Bytes:
            return KeyInput{key.data};
    }

    return std::nullopt;
}

std::optional<ValueInput> decode_value_input(const Value &value) {
    /**
     * Convert a persisted Value into the public ValueInput variant.
     *
     * The codec validates the outer type/size shape first. Integer helpers then
     * reject malformed or overflowing varints. Bool gets one extra canonical
     * byte check here because the public API only admits false (0) and true (1).
     */
    if (!valuecodec::validate_value(value)) return std::nullopt;

    switch (value.type) {
        case ValueType::VarUInt: {
            std::uint64_t decoded = 0;
            if (!valuecodec::decode_varuint(value, &decoded)) return std::nullopt;
            return ValueInput{decoded};
        }
        case ValueType::VarInt: {
            std::int64_t decoded = 0;
            if (!valuecodec::decode_varint(value, &decoded)) return std::nullopt;
            return ValueInput{decoded};
        }
        case ValueType::Bool:
            if (value.data[0] != '\0' && value.data[0] != '\1') {
                return std::nullopt;
            }
            return ValueInput{value.data[0] == '\1'};
        case ValueType::Char:
            return ValueInput{std::string(value.data.begin(), value.data.end())};
    }

    return std::nullopt;
}

} // namespace

// ================================= KeyStoreCursor =================================

KeyStoreCursor::KeyStoreCursor(
    BTreeCursor cursor,
    std::optional<Key> exclusive_end,
    std::optional<Key> required_prefix
) : cursor(std::move(cursor)),
    exclusive_end(std::move(exclusive_end)),
    required_prefix(std::move(required_prefix)) {
    /**
     * The BTree cursor arrives already positioned by KeyStore::scan*.
     *
     * A seek can succeed while landing directly on the exclusive end key or on
     * the first key after a requested prefix. Record that as a sticky logical
     * end immediately; the underlying cursor remains alive until close so its
     * pinned page and BTree registration still have one clear owner.
     */
    if (!this->cursor.valid()) return;

    BTreeCursorResult result = this->cursor.current();
    if (
        result.status == BTreeCursorStatus::Success &&
        !current_key_is_in_bounds(result.key)
    ) {
        exhausted_by_bound = true;
    }
}

KeyStoreCursorResult KeyStoreCursor::current() const {
    /**
     * Return the current logical entry without moving the scan.
     *
     * Bound exhaustion is checked before decoding so a range never exposes its
     * end key and a prefix scan never exposes the first nonmatching key. Raw
     * cursor failures stay storage errors, while a well-read but malformed
     * persisted key/value pair is reported as DecodeFailed.
     */
    KeyStoreCursorResult result{};
    if (exhausted_by_bound) {
        result.status = KeyStoreCursorStatus::EndOfScan;
        return result;
    }

    BTreeCursorResult storage_result = cursor.current();
    result.status = translate_cursor_status(storage_result.status);
    if (storage_result.status != BTreeCursorStatus::Success) return result;

    if (!current_key_is_in_bounds(storage_result.key)) {
        result.status = KeyStoreCursorStatus::EndOfScan;
        return result;
    }

    std::optional<KeyInput> key = decode_key(storage_result.key);
    std::optional<ValueInput> value = decode_value(storage_result.value);
    if (!key.has_value() || !value.has_value()) {
        result.status = KeyStoreCursorStatus::DecodeFailed;
        return result;
    }

    result.status = KeyStoreCursorStatus::Success;
    result.entry = KeyStoreEntry{std::move(*key), std::move(*value)};
    return result;
}

KeyStoreCursorStatus KeyStoreCursor::next() {
    /**
     * Advance by one physical BTree key, then apply the logical scan bound.
     *
     * Once a range or prefix bound is crossed, EndOfScan is sticky. This mirrors
     * BTreeCursor's sticky EndOfTree behavior and prevents a caller from
     * advancing out of one logical scan into a later key family.
     */
    if (exhausted_by_bound) return KeyStoreCursorStatus::EndOfScan;

    BTreeCursorStatus next_status = cursor.next();
    if (next_status != BTreeCursorStatus::Success) {
        return translate_cursor_status(next_status);
    }

    BTreeCursorResult storage_result = cursor.current();
    if (storage_result.status != BTreeCursorStatus::Success) {
        return translate_cursor_status(storage_result.status);
    }

    if (!current_key_is_in_bounds(storage_result.key)) {
        exhausted_by_bound = true;
        return KeyStoreCursorStatus::EndOfScan;
    }

    return KeyStoreCursorStatus::Success;
}

bool KeyStoreCursor::valid() const {
    // BTree validity alone is insufficient: a positioned physical cursor can
    // already be outside this wrapper's exclusive-end or prefix bound.
    if (exhausted_by_bound || !cursor.valid()) return false;

    BTreeCursorResult result = cursor.current();
    return result.status == BTreeCursorStatus::Success &&
        current_key_is_in_bounds(result.key);
}

KeyStoreCursorStatus KeyStoreCursor::close() {
    // BTreeCursor::close is idempotent and owns the actual page unpin plus BTree
    // unregister operation. Clear the wrapper-only end marker after it succeeds
    // so later current() calls correctly report Closed instead of EndOfScan.
    BTreeCursorStatus close_status = cursor.close();
    if (close_status == BTreeCursorStatus::Success) exhausted_by_bound = false;
    return translate_cursor_status(close_status);
}

bool KeyStoreCursor::current_key_is_in_bounds(const Key &key) const {
    // Range scans are half-open: the encoded end key is never part of the result.
    if (
        exclusive_end.has_value() &&
        keycodec::compare(key, *exclusive_end) >= 0
    ) {
        return false;
    }

    if (required_prefix.has_value()) {
        // Key types participate in global ordering. Matching payload bytes is
        // not enough: string "a" and byte-vector {'a'} are different families.
        if (key.type != required_prefix->type) return false;
        if (key.data.size() < required_prefix->data.size()) return false;
        if (!std::equal(
            required_prefix->data.begin(),
            required_prefix->data.end(),
            key.data.begin()
        )) {
            return false;
        }
    }

    return true;
}

KeyStoreCursorStatus KeyStoreCursor::translate_cursor_status(
    BTreeCursorStatus status
) {
    // Hide storage-specific failure details from the client cursor API while
    // preserving the distinctions a caller can act on.
    switch (status) {
        case BTreeCursorStatus::Success:
            return KeyStoreCursorStatus::Success;
        case BTreeCursorStatus::EndOfTree:
            return KeyStoreCursorStatus::EndOfScan;
        case BTreeCursorStatus::NotPositioned:
            return KeyStoreCursorStatus::NotPositioned;
        case BTreeCursorStatus::Closed:
            return KeyStoreCursorStatus::Closed;
        case BTreeCursorStatus::FailedToRead:
        case BTreeCursorStatus::FailedToReleasePage:
            return KeyStoreCursorStatus::StorageError;
    }

    return KeyStoreCursorStatus::StorageError;
}

std::optional<KeyInput> KeyStoreCursor::decode_key(const Key &key) {
    // Keep cursor decoding as a class helper while sharing the actual codec
    // policy with point operations through the file-local implementation.
    return decode_key_input(key);
}

std::optional<ValueInput> KeyStoreCursor::decode_value(const Value &value) {
    // A scan must surface exactly the same ValueInput type that get() would.
    return decode_value_input(value);
}

// ==================================== KeyStore ====================================

KeyStore::KeyStore(KeyStoreOptions options) : options(options) {}

KeyStore::~KeyStore() {
    /**
     * Destruction is the last-resort cleanup path and cannot return a status.
     *
     * Public close() intentionally refuses to discard an unresolved explicit
     * transaction. A destructor has no caller left to resolve it, so it makes a
     * best-effort rollback before closing the BTree. As documented in the
     * header, every KeyStoreCursor must already be destroyed because it holds a
     * pointer into this object's BTree.
     */
    if (!is_open) return;

    if (transaction_state != TransactionState::None) {
        tree.rollback();
        clear_transaction_state();
    }

    tree.close();
    is_open = false;
}

KeyStoreStatus KeyStore::open(const std::string &db_file) {
    // One KeyStore object owns at most one live BTree/Pager session. Reopening
    // the same object is allowed only after a successful close.
    if (is_open) return KeyStoreStatus::AlreadyOpen;

    BTreeStatus open_status = tree.open(db_file);
    if (open_status != BTreeStatus::Success) return KeyStoreStatus::FailedToOpen;

    is_open = true;
    clear_transaction_state();
    return KeyStoreStatus::Success;
}

KeyStoreStatus KeyStore::close() {
    /**
     * Close only a transactionally clean KeyStore.
     *
     * Explicit close never makes an implicit commit/rollback decision for the
     * caller. A failed transaction must be rolled back, an active transaction
     * must be committed or rolled back, and every cursor must release its page
     * registration before the BTree can close safely.
     */
    if (!is_open) return KeyStoreStatus::Success;
    if (transaction_state == TransactionState::Failed) {
        return KeyStoreStatus::TransactionNeedsRollback;
    }
    if (transaction_state == TransactionState::Active) {
        return KeyStoreStatus::WriteTransactionActive;
    }

    BTreeStatus close_status = tree.close();
    if (close_status == BTreeStatus::CursorActive) {
        return KeyStoreStatus::CursorActive;
    }

    // BTree::close tears down its pager even when that teardown reports failure.
    is_open = false;
    clear_transaction_state();
    if (close_status != BTreeStatus::Success) {
        return KeyStoreStatus::FailedToClose;
    }

    return KeyStoreStatus::Success;
}

KeyStoreGetResult KeyStore::get(const KeyInput &key) {
    /**
     * Point lookup flow:
     *
     * 1. Validate and encode the client key.
     * 2. Ask the BTree for the encoded value.
     * 3. Translate absence separately from storage failure.
     * 4. Decode the stored value into the public variant.
     *
     * Reads are allowed during explicit write transactions so a transaction can
     * observe its own uncommitted page-cache changes.
     */
    KeyStoreGetResult result{};
    if (!is_open) {
        result.status = KeyStoreStatus::NotOpen;
        return result;
    }

    std::optional<Key> encoded_key = encode_key(key);
    if (!encoded_key.has_value()) {
        result.status = KeyStoreStatus::InvalidKey;
        return result;
    }

    BTreeGetStatus get_result = tree.get(*encoded_key);
    if (get_result.status == BTreeStatus::KeyNotInTree) {
        result.status = KeyStoreStatus::KeyNotFound;
        return result;
    }
    if (get_result.status != BTreeStatus::Success) {
        result.status = KeyStoreStatus::ReadFailed;
        return result;
    }

    result.value = decode_value(get_result.value);
    if (!result.value.has_value()) {
        result.status = KeyStoreStatus::DecodeFailed;
        return result;
    }

    result.status = KeyStoreStatus::Success;
    return result;
}

KeyStoreStatus KeyStore::put(
    const KeyInput &key,
    const ValueInput &value
) {
    /**
     * Insert or overwrite a typed key/value pair.
     *
     * Encoding happens before transaction checks because InvalidKey and
     * InvalidValue are caller errors and must never poison a transaction. Once
     * BTree::insert runs, completion is delegated to either the explicit
     * transaction state machine or the autocommit commit/rollback path.
     */
    if (!is_open) return KeyStoreStatus::NotOpen;

    std::optional<Key> encoded_key = encode_key(key);
    if (!encoded_key.has_value()) return KeyStoreStatus::InvalidKey;

    std::optional<Value> encoded_value = encode_value(value);
    if (!encoded_value.has_value()) return KeyStoreStatus::InvalidValue;

    if (transaction_state == TransactionState::Failed) {
        // A previous storage/commit failure may have left dirty state. No later
        // write is safe until rollback restores the transaction boundary.
        return KeyStoreStatus::TransactionNeedsRollback;
    }
    if (
        transaction_state == TransactionState::None &&
        options.write_policy == KeyStoreWritePolicy::ExplicitTransactionsOnly
    ) {
        return KeyStoreStatus::NoActiveTransaction;
    }

    BTreeStatus write_status = tree.insert(*encoded_key, *encoded_value);
    // An explicit transaction retains successful dirty pages for a later
    // commit. With no explicit transaction, this one call owns the full write.
    if (transaction_state == TransactionState::Active) {
        return handle_transactional_write(write_status);
    }

    return finish_autocommit_write(write_status);
}

KeyStoreRemoveResult KeyStore::remove(const KeyInput &key) {
    /**
     * Delete a key and return the value that was stored under it.
     *
     * The raw value must be decoded before an autocommit delete is finalized.
     * Otherwise a decode failure could commit the deletion while preventing the
     * API from returning the promised previous value. In that case explicit
     * transactions enter Failed and autocommit rolls the deletion back.
     */
    KeyStoreRemoveResult result{};
    if (!is_open) {
        result.status = KeyStoreStatus::NotOpen;
        return result;
    }

    std::optional<Key> encoded_key = encode_key(key);
    if (!encoded_key.has_value()) {
        result.status = KeyStoreStatus::InvalidKey;
        return result;
    }

    if (transaction_state == TransactionState::Failed) {
        result.status = KeyStoreStatus::TransactionNeedsRollback;
        return result;
    }
    if (
        transaction_state == TransactionState::None &&
        options.write_policy == KeyStoreWritePolicy::ExplicitTransactionsOnly
    ) {
        result.status = KeyStoreStatus::NoActiveTransaction;
        return result;
    }

    BTreeRemoveStatus remove_result = tree.remove(*encoded_key);
    if (remove_result.status == BTreeStatus::KeyNotInTree) {
        // Not-found is a clean logical result: no page was changed, so there is
        // nothing to commit, roll back, or mark as a failed transaction.
        result.status = KeyStoreStatus::KeyNotFound;
        return result;
    }

    if (remove_result.status != BTreeStatus::Success) {
        result.status = (transaction_state == TransactionState::Active)
            ? handle_transactional_write(remove_result.status)
            : finish_autocommit_write(remove_result.status);
        return result;
    }

    std::optional<ValueInput> removed_value = decode_value(remove_result.value);
    if (!removed_value.has_value()) {
        if (transaction_state == TransactionState::Active) {
            // The BTree mutation already happened in this transaction. Keep the
            // transaction alive but force the caller through rollback.
            transaction_state = TransactionState::Failed;
            result.status = KeyStoreStatus::DecodeFailed;
            return result;
        }

        // Autocommit cannot publish a delete whose return value could not be
        // decoded. Restore the page state before reporting DecodeFailed.
        BTreeRollbackStatus rollback_status = tree.rollback();
        result.status = (rollback_status == BTreeRollbackStatus::Success)
            ? KeyStoreStatus::DecodeFailed
            : KeyStoreStatus::RollbackFailed;
        return result;
    }

    result.status = (transaction_state == TransactionState::Active)
        ? handle_transactional_write(BTreeStatus::Success)
        : finish_autocommit_write(BTreeStatus::Success);
    if (result.status == KeyStoreStatus::Success) {
        result.value = std::move(*removed_value);
    }
    return result;
}

KeyStoreStatus KeyStore::begin_write_transaction() {
    /**
     * Begin a logical explicit transaction.
     *
     * The Pager acquires write-side locks lazily on the first actual mutation,
     * so beginning an empty transaction changes only KeyStore state.
     */
    if (!is_open) return KeyStoreStatus::NotOpen;
    if (transaction_state == TransactionState::Failed) {
        return KeyStoreStatus::TransactionNeedsRollback;
    }
    if (transaction_state == TransactionState::Active) {
        return KeyStoreStatus::TransactionAlreadyActive;
    }

    transaction_state = TransactionState::Active;
    transaction_has_writes = false;
    return KeyStoreStatus::Success;
}

KeyStoreStatus KeyStore::commit() {
    /**
     * Commit the active explicit transaction.
     *
     * Empty transactions are completed locally because the Pager has no dirty
     * state or write lock to publish. Transactions with writes delegate to the
     * BTree two-phase commit. A storage commit failure changes the state to
     * Failed because only rollback can establish a safe next boundary.
     */
    if (!is_open) return KeyStoreStatus::NotOpen;
    if (transaction_state == TransactionState::None) {
        return KeyStoreStatus::NoActiveTransaction;
    }
    if (transaction_state == TransactionState::Failed) {
        return KeyStoreStatus::TransactionNeedsRollback;
    }
    // A cursor pins a leaf and commit may flush/release the pages beneath it.
    if (tree.cursor_active()) return KeyStoreStatus::CursorActive;
    if (!transaction_has_writes) {
        clear_transaction_state();
        return KeyStoreStatus::Success;
    }

    BTreeCommitStatus commit_status = tree.commit();
    if (commit_status == BTreeCommitStatus::CursorActive) {
        return KeyStoreStatus::CursorActive;
    }
    if (commit_status != BTreeCommitStatus::Success) {
        transaction_state = TransactionState::Failed;
        return KeyStoreStatus::CommitFailed;
    }

    clear_transaction_state();
    return KeyStoreStatus::Success;
}

KeyStoreStatus KeyStore::rollback() {
    /**
     * Roll back either an active or failed explicit transaction.
     *
     * Failed transactions always reach BTree::rollback, even if no successful
     * write was recorded: the failed BTree operation may have partially dirtied
     * storage state before returning. A clean empty transaction can be cleared
     * locally.
     */
    if (!is_open) return KeyStoreStatus::NotOpen;
    if (transaction_state == TransactionState::None) {
        return KeyStoreStatus::NoActiveTransaction;
    }
    // Rollback may invalidate cached pages owned by a cursor.
    if (tree.cursor_active()) return KeyStoreStatus::CursorActive;
    if (
        transaction_state == TransactionState::Active &&
        !transaction_has_writes
    ) {
        clear_transaction_state();
        return KeyStoreStatus::Success;
    }

    BTreeRollbackStatus rollback_status = tree.rollback();
    if (rollback_status == BTreeRollbackStatus::CursorActive) {
        return KeyStoreStatus::CursorActive;
    }
    if (rollback_status != BTreeRollbackStatus::Success) {
        transaction_state = TransactionState::Failed;
        return KeyStoreStatus::RollbackFailed;
    }

    clear_transaction_state();
    return KeyStoreStatus::Success;
}

bool KeyStore::write_transaction_active() const {
    // Failed still counts as active: its locks and dirty state are not resolved
    // until rollback succeeds.
    return transaction_state != TransactionState::None;
}

// ====================================== Scans =====================================

KeyStoreScanResult KeyStore::scan() {
    /**
     * Open a full ordered scan.
     *
     * BTree cursors start unpositioned, so KeyStore positions on the leftmost
     * key before transferring ownership to KeyStoreCursor. EndOfTree is not a
     * scan-construction failure; it creates a valid empty logical cursor.
     */
    KeyStoreScanResult result{};
    if (!is_open) {
        result.status = KeyStoreStatus::NotOpen;
        return result;
    }

    BTreeCursor cursor = tree.open_cursor();
    BTreeCursorStatus seek_status = cursor.seek_first();
    if (
        seek_status != BTreeCursorStatus::Success &&
        seek_status != BTreeCursorStatus::EndOfTree
    ) {
        result.status = KeyStoreStatus::ScanFailed;
        return result;
    }

    // Construct the private wrapper here, then move it into optional. The
    // wrapper now owns the BTree cursor registration and any pinned leaf.
    result.cursor.emplace(KeyStoreCursor(std::move(cursor)));
    result.status = KeyStoreStatus::Success;
    return result;
}

KeyStoreScanResult KeyStore::scan_from(const KeyInput &start) {
    /**
     * Open a lower-bound scan over [start, end of tree).
     *
     * BTreeCursor::seek already implements "first key >= target", including
     * crossing a leaf boundary when the target is absent from its search leaf.
     */
    KeyStoreScanResult result{};
    if (!is_open) {
        result.status = KeyStoreStatus::NotOpen;
        return result;
    }

    std::optional<Key> encoded_start = encode_key(start);
    if (!encoded_start.has_value()) {
        result.status = KeyStoreStatus::InvalidKey;
        return result;
    }

    BTreeCursor cursor = tree.open_cursor();
    BTreeCursorStatus seek_status = cursor.seek(*encoded_start);
    if (
        seek_status != BTreeCursorStatus::Success &&
        seek_status != BTreeCursorStatus::EndOfTree
    ) {
        result.status = KeyStoreStatus::ScanFailed;
        return result;
    }

    result.cursor.emplace(KeyStoreCursor(std::move(cursor)));
    result.status = KeyStoreStatus::Success;
    return result;
}

KeyStoreScanResult KeyStore::scan_range(
    const KeyInput &start,
    const KeyInput &end
) {
    /**
     * Open a half-open ordered range [start, end).
     *
     * KeyCodec defines a total order across key families, so heterogeneous
     * ranges are valid. Only start > end is invalid; start == end is a valid
     * empty range and is represented by a bound-exhausted cursor.
     */
    KeyStoreScanResult result{};
    if (!is_open) {
        result.status = KeyStoreStatus::NotOpen;
        return result;
    }

    std::optional<Key> encoded_start = encode_key(start);
    std::optional<Key> encoded_end = encode_key(end);
    if (!encoded_start.has_value() || !encoded_end.has_value()) {
        result.status = KeyStoreStatus::InvalidKey;
        return result;
    }
    if (keycodec::compare(*encoded_start, *encoded_end) > 0) {
        result.status = KeyStoreStatus::InvalidRange;
        return result;
    }

    BTreeCursor cursor = tree.open_cursor();
    BTreeCursorStatus seek_status = cursor.seek(*encoded_start);
    if (
        seek_status != BTreeCursorStatus::Success &&
        seek_status != BTreeCursorStatus::EndOfTree
    ) {
        result.status = KeyStoreStatus::ScanFailed;
        return result;
    }

    // The physical cursor is positioned only at the lower bound. The wrapper
    // owns the exclusive upper-bound check on current() and next().
    result.cursor.emplace(KeyStoreCursor(
        std::move(cursor),
        std::move(encoded_end)
    ));
    result.status = KeyStoreStatus::Success;
    return result;
}

KeyStoreScanResult KeyStore::scan_prefix(const KeyPrefix &prefix) {
    /**
     * Open a prefix scan for string or byte-vector keys.
     *
     * Seeking to the encoded prefix finds the first possible match because both
     * variable-length key families use lexicographic payload ordering. The
     * wrapper stores the encoded prefix and ends the scan as soon as either the
     * key family changes or the payload no longer begins with those bytes.
     */
    KeyStoreScanResult result{};
    if (!is_open) {
        result.status = KeyStoreStatus::NotOpen;
        return result;
    }

    // Reuse the normal key encoder so prefix scans follow the same type tags,
    // payload-size validation, and byte representation as point operations.
    KeyInput prefix_input = std::visit(
        [](const auto &value) -> KeyInput { return value; },
        prefix
    );
    std::optional<Key> encoded_prefix = encode_key(prefix_input);
    if (!encoded_prefix.has_value()) {
        result.status = KeyStoreStatus::InvalidKey;
        return result;
    }

    BTreeCursor cursor = tree.open_cursor();
    BTreeCursorStatus seek_status = cursor.seek(*encoded_prefix);
    if (
        seek_status != BTreeCursorStatus::Success &&
        seek_status != BTreeCursorStatus::EndOfTree
    ) {
        result.status = KeyStoreStatus::ScanFailed;
        return result;
    }

    result.cursor.emplace(KeyStoreCursor(
        std::move(cursor),
        std::nullopt,
        std::move(encoded_prefix)
    ));
    result.status = KeyStoreStatus::Success;
    return result;
}

// =============================== Encoding + Writes ================================

std::optional<Key> KeyStore::encode_key(const KeyInput &key) {
    /**
     * Map one public key alternative to its order-preserving storage encoding.
     *
     * std::visit keeps this dispatch exhaustive: adding a KeyInput alternative
     * requires adding its codec branch here. Variable payloads are checked
     * before the codec narrows their size into the on-disk representation.
     */
    return std::visit(
        [](const auto &value) -> std::optional<Key> {
            using T = std::decay_t<decltype(value)>;

            if constexpr (std::is_same_v<T, bool>) {
                return keycodec::make_bool(value);
            } else if constexpr (std::is_same_v<T, std::uint64_t>) {
                return keycodec::make_uint64(value);
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                return keycodec::make_int64(value);
            } else if constexpr (std::is_same_v<T, std::string>) {
                if (value.size() > MAX_ENCODED_PAYLOAD_SIZE) {
                    return std::nullopt;
                }
                return keycodec::make_string(value);
            } else {
                if (value.size() > MAX_ENCODED_PAYLOAD_SIZE) {
                    return std::nullopt;
                }
                return keycodec::make_bytes(value);
            }
        },
        key
    );
}

std::optional<Value> KeyStore::encode_value(const ValueInput &value) {
    /**
     * Map one public value alternative to its persisted Value representation.
     *
     * Unsigned integers use VarUInt, signed integers use ZigZag + VarInt, bool
     * is one canonical byte, and public strings map to the current Char type.
     */
    return std::visit(
        [](const auto &input) -> std::optional<Value> {
            using T = std::decay_t<decltype(input)>;

            if constexpr (std::is_same_v<T, bool>) {
                return valuecodec::make_bool(input);
            } else if constexpr (std::is_same_v<T, std::uint64_t>) {
                return valuecodec::make_varuint(input);
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                return valuecodec::make_varint(input);
            } else {
                if (input.size() > MAX_ENCODED_PAYLOAD_SIZE) {
                    return std::nullopt;
                }
                return valuecodec::make_char(input);
            }
        },
        value
    );
}

std::optional<ValueInput> KeyStore::decode_value(const Value &value) {
    // Point operations and cursors intentionally share one decoding policy so
    // the same persisted bytes cannot be interpreted differently by get/scan.
    return decode_value_input(value);
}

KeyStoreStatus KeyStore::finish_autocommit_write(BTreeStatus write_status) {
    /**
     * Finish the write unit owned by one autocommit put/remove call.
     *
     * State machine:
     *
     *   BTree write failed -> rollback -> WriteFailed / RollbackFailed
     *   BTree write passed -> commit
     *   commit passed      -> Success
     *   commit failed      -> rollback -> CommitFailed / RollbackFailed
     *
     * CursorActive is known to occur before BTree mutation, so it can be
     * returned directly without rollback. Every other lower-level failure is
     * conservatively treated as potentially dirty.
     */
    if (write_status == BTreeStatus::CursorActive) {
        return KeyStoreStatus::CursorActive;
    }
    if (write_status != BTreeStatus::Success) {
        BTreeRollbackStatus rollback_status = tree.rollback();
        return (rollback_status == BTreeRollbackStatus::Success)
            ? KeyStoreStatus::WriteFailed
            : KeyStoreStatus::RollbackFailed;
    }

    BTreeCommitStatus commit_status = tree.commit();
    if (commit_status == BTreeCommitStatus::Success) {
        return KeyStoreStatus::Success;
    }

    // Commit failure must not leak an unresolved implicit transaction into the
    // next API call. Roll back before translating the original failure.
    BTreeRollbackStatus rollback_status = tree.rollback();
    if (rollback_status != BTreeRollbackStatus::Success) {
        return KeyStoreStatus::RollbackFailed;
    }
    if (commit_status == BTreeCommitStatus::CursorActive) {
        return KeyStoreStatus::CursorActive;
    }
    return KeyStoreStatus::CommitFailed;
}

KeyStoreStatus KeyStore::handle_transactional_write(BTreeStatus write_status) {
    /**
     * Record the outcome of a write inside an explicit transaction.
     *
     * Successful writes remain dirty until commit/rollback. CursorActive is a
     * pre-mutation conflict and leaves the transaction usable. Any other BTree
     * failure may have partially changed transaction-owned state, so future
     * writes and commit are blocked until rollback.
     */
    if (write_status == BTreeStatus::CursorActive) {
        return KeyStoreStatus::CursorActive;
    }
    if (write_status != BTreeStatus::Success) {
        transaction_state = TransactionState::Failed;
        return KeyStoreStatus::WriteFailed;
    }

    transaction_has_writes = true;
    return KeyStoreStatus::Success;
}

void KeyStore::clear_transaction_state() {
    // Call only after a successful commit/rollback or an empty transaction.
    // Keeping both fields reset together avoids a stale has-writes bit leaking
    // into the next explicit transaction.
    transaction_state = TransactionState::None;
    transaction_has_writes = false;
}

#include <KeyStore.h>

#include <KeyCodec.h>
#include <ValueCodec.h>

#include <algorithm>
#include <cstdint>
#include <utility>

/**
 * KeyStore is the validated key-value layer above BTree.
 *
 * The BTree deliberately works with encoded Key and Value objects and exposes
 * storage-oriented status values. KeyStore is responsible for the opposite
 * side of that boundary:
 *
 * - validate encoded keys and values before they reach the BTree;
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

    if (
        !KeyCodec::validate_key(storage_result.key) ||
        !ValueCodec::validate_value(storage_result.value)
    ) {
        result.status = KeyStoreCursorStatus::DecodeFailed;
        return result;
    }

    result.status = KeyStoreCursorStatus::Success;
    result.entry = KeyStoreEntry{
        std::move(storage_result.key),
        std::move(storage_result.value)
    };
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
        KeyCodec::compare(key, *exclusive_end) >= 0
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
    // Hide storage-specific failure details from the caller-facing cursor API while
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

KeyStoreGetResult KeyStore::get(const Key &key) {
    /**
     * Point lookup flow:
     *
     * 1. Validate the encoded key.
     * 2. Ask the BTree for the encoded value.
     * 3. Translate absence separately from storage failure.
     * 4. Validate the stored value before exposing it.
     *
     * Reads are allowed during explicit write transactions so a transaction can
     * observe its own uncommitted page-cache changes.
     */
    KeyStoreGetResult result{};
    if (!is_open) {
        result.status = KeyStoreStatus::NotOpen;
        return result;
    }

    if (!KeyCodec::validate_key(key)) {
        result.status = KeyStoreStatus::InvalidKey;
        return result;
    }

    BTreeGetStatus get_result = tree.get(key);
    if (get_result.status == BTreeStatus::KeyNotInTree) {
        result.status = KeyStoreStatus::KeyNotFound;
        return result;
    }
    if (get_result.status != BTreeStatus::Success) {
        result.status = KeyStoreStatus::ReadFailed;
        return result;
    }

    if (!ValueCodec::validate_value(get_result.value)) {
        result.status = KeyStoreStatus::DecodeFailed;
        return result;
    }

    result.status = KeyStoreStatus::Success;
    result.value = std::move(get_result.value);
    return result;
}

KeyStoreStatus KeyStore::put(
    const Key &key,
    const Value &value
) {
    /**
     * Insert or overwrite a typed key/value pair.
     *
     * Validation happens before transaction checks because InvalidKey and
     * InvalidValue are caller errors and must never poison a transaction. Once
     * BTree::insert runs, completion is delegated to either the explicit
     * transaction state machine or the autocommit commit/rollback path.
     */
    if (!is_open) return KeyStoreStatus::NotOpen;

    if (!KeyCodec::validate_key(key)) return KeyStoreStatus::InvalidKey;
    if (!ValueCodec::validate_value(value)) return KeyStoreStatus::InvalidValue;

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

    Value stored_value = value;
    BTreeStatus write_status = tree.insert(key, stored_value);
    // An explicit transaction retains successful dirty pages for a later
    // commit. With no explicit transaction, this one call owns the full write.
    if (transaction_state == TransactionState::Active) {
        return handle_transactional_write(write_status);
    }

    return finish_autocommit_write(write_status);
}

KeyStoreRemoveResult KeyStore::remove(const Key &key) {
    /**
     * Delete a key and return the value that was stored under it.
     *
     * The raw value must be validated before an autocommit delete is finalized.
     * Otherwise invalid stored data could commit the deletion while preventing the
     * API from returning the promised previous value. In that case explicit
     * transactions enter Failed and autocommit rolls the deletion back.
     */
    KeyStoreRemoveResult result{};
    if (!is_open) {
        result.status = KeyStoreStatus::NotOpen;
        return result;
    }

    if (!KeyCodec::validate_key(key)) {
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

    BTreeRemoveStatus remove_result = tree.remove(key);
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

    if (!ValueCodec::validate_value(remove_result.value)) {
        if (transaction_state == TransactionState::Active) {
            // The BTree mutation already happened in this transaction. Keep the
            // transaction alive but force the caller through rollback.
            transaction_state = TransactionState::Failed;
            result.status = KeyStoreStatus::DecodeFailed;
            return result;
        }

        // Autocommit cannot publish a delete whose return value is invalid.
        // Restore the page state before reporting DecodeFailed.
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
        result.value = std::move(remove_result.value);
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

KeyStoreScanResult KeyStore::scan_from(const Key &start) {
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

    if (!KeyCodec::validate_key(start)) {
        result.status = KeyStoreStatus::InvalidKey;
        return result;
    }

    BTreeCursor cursor = tree.open_cursor();
    BTreeCursorStatus seek_status = cursor.seek(start);
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
    const Key &start,
    const Key &end
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

    if (!KeyCodec::validate_key(start) || !KeyCodec::validate_key(end)) {
        result.status = KeyStoreStatus::InvalidKey;
        return result;
    }
    if (KeyCodec::compare(start, end) > 0) {
        result.status = KeyStoreStatus::InvalidRange;
        return result;
    }

    BTreeCursor cursor = tree.open_cursor();
    BTreeCursorStatus seek_status = cursor.seek(start);
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
        end
    ));
    result.status = KeyStoreStatus::Success;
    return result;
}

KeyStoreScanResult KeyStore::scan_prefix(const Key &prefix) {
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

    if (
        !KeyCodec::validate_key(prefix) ||
        (prefix.type != KeyType::String && prefix.type != KeyType::Bytes)
    ) {
        result.status = KeyStoreStatus::InvalidKey;
        return result;
    }

    BTreeCursor cursor = tree.open_cursor();
    BTreeCursorStatus seek_status = cursor.seek(prefix);
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
        prefix
    ));
    result.status = KeyStoreStatus::Success;
    return result;
}

// ==================================== Writes ======================================

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

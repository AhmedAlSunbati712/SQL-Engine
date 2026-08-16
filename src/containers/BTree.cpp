#include <BTree.h>
#include <BTreeCursor.h>
#include <KeyCodec.h>
#include <Pager.h>
#include <algorithm>
#include <cassert>
#include <utility>

namespace {

void record_page_effect(
    PendingBTreeAction *action,
    PageEffectKind kind,
    const PageV2 *page
) {
    if (!action) return;
    PageEffect effect{
        .kind = kind,
        .page_num = page->page_num,
    };
    effect.after_image = page->data;
    action->add_effect(std::move(effect));
}

void record_page_effect(
    PendingBTreeAction *action,
    PageEffectKind kind,
    std::uint32_t page_num,
    const char *page_data
) {
    if (!action) return;
    PageEffect effect{
        .kind = kind,
        .page_num = page_num,
    };
    std::copy_n(page_data, V2_PAGE_SIZE, effect.after_image.begin());
    action->add_effect(std::move(effect));
}

void record_pager_effects(
    PendingBTreeAction *action,
    std::vector<PageEffect> effects
) {
    if (!action) return;
    for (PageEffect& effect : effects) {
        action->add_effect(std::move(effect));
    }
}

} // namespace

BTree::~BTree() {
    // A cursor holds a pointer back to this BTree, so the BTree must always outlive it.
    assert(active_cursor_count == 0);
    close();
}

void BTree::register_cursor() {
    // A cursor can only exist while this BTree has a live pager behind it.
    assert(pager_open && pager != nullptr);
    active_cursor_count++;
}

void BTree::unregister_cursor() {
    // Every cursor registration must be released exactly once.
    assert(active_cursor_count > 0);
    active_cursor_count--;
}

BTreeCursor BTree::open_cursor() {
    // Like the rest of the BTree data API, cursor creation requires an open database.
    assert(pager_open && pager != nullptr);
    return BTreeCursor(this);
}

bool BTree::cursor_active() const {
    return active_cursor_count > 0;
}

BTreeStatus BTree::open(std::string db_file) {
    assert(!pager_open && pager == nullptr);

    pager = new Pager();
    if (transaction_manager) pager->attach_log(transaction_manager->log());
    PagerResult open_result = pager->open(db_file);
    if (open_result != PagerResult::Success) {
        delete pager;
        pager = nullptr;
        return BTreeStatus::FailedToOpenDB;
    }

    currently_open_db_file = std::move(db_file);
    pager_open = true;
    storage_poisoned = false;
    return BTreeStatus::Success;
}

BTreeStatus BTree::close() {
    // If we are already closed, there is nothing to do.
    if (!pager_open && pager == nullptr) return BTreeStatus::Success;

    // Closing the pager would invalidate every page and path owned by a cursor.
    if (active_cursor_count > 0) return BTreeStatus::CursorActive;

    // If the client closes while a write txn is still alive, try to roll it back first.
    // Even if rollback fails, we still tear down the pager and report the failure.
    BTreeStatus close_status = BTreeStatus::Success;
    if (pager_open && pager) {
        PagerResult rollback_result = pager->rollback_transaction();
        if (rollback_result != PagerResult::Success) {
            close_status = BTreeStatus::FailedToCloseDB;
        }
    }

    // The BTree owns the pager. Delete it and reset the local session state
    // so this object can open another database later.
    if (pager) {
        delete pager;
        pager = nullptr;
    }

    pager_open = false;
    currently_open_db_file.clear();
    return close_status;
}

BTreeCommitStatus BTree::commit() {
    if (!pager_open || !pager) return BTreeCommitStatus::Failed;

    // A commit can release locks and refresh pager state, which would invalidate a cursor.
    if (active_cursor_count > 0) return BTreeCommitStatus::CursorActive;

    // Phase one makes the journal durable and pushes dirty pages to the db file.
    PagerResult phase_one_result = pager->commit_phase_one();
    if (phase_one_result != PagerResult::Success) return BTreeCommitStatus::Failed;

    // Phase two truncates the journal and releases / downgrades locks as needed.
    PagerResult phase_two_result = pager->commit_phase_two();
    if (phase_two_result != PagerResult::Success) return BTreeCommitStatus::Failed;

    return BTreeCommitStatus::Success;
}

BTreeRollbackStatus BTree::rollback() {
    if (!pager_open || !pager) return BTreeRollbackStatus::Failed;

    // Rollback can remove cached pages that an active cursor still references.
    if (active_cursor_count > 0) return BTreeRollbackStatus::CursorActive;

    // Let the pager unwind either the in-memory write set or the durable journal.
    PagerResult rollback_result = pager->rollback_transaction();
    if (rollback_result != PagerResult::Success) return BTreeRollbackStatus::Failed;

    return BTreeRollbackStatus::Success;
}


BTreeGetStatus BTree::get(const Key &key) {
    /**
     * status <- BTreeGetStatus{}
     * status.status <- success
     *
     * # Shared helper used by get / insert / remove
     * # descend_from_root_to_leaf(key) returns:
     * #   - leaf_page
     * #   - leaf_page_num
     * #   - path[]
     * #
     * # each entry in path stores enough metadata to repair upward later:
     * #   - parent_page_num
     * #   - child_index_taken
     * #   - separator_index_used
     *
     * descent <- descend_from_root_to_leaf(key)
     * if descent failed, return failure status
     *
     * leaf <- BTreeLeafPage(descent.leaf_page.data)
     * idx <- leaf.lower_bound_key(key)
     * if idx is out of bounds or leaf.keys[idx] != key {
     *      pager->unref(descent.leaf_page_num)
     *      status.status <- not_found
     *      return status
     * }
     *
     * status.value <- leaf.values[idx]
     * pager->unref(descent.leaf_page_num)
     * return status
    */
    BTreeGetStatus get_result{};
    if (storage_poisoned) {
        get_result.status = BTreeStatus::FailedToRead;
        return get_result;
    }
    get_result.status = BTreeStatus::Success;
    BTreeOperation operation(0, pager->page_latch_manager());

    // Descend from the root tohe leaf. We pass false to the second arg
    // Since we are not interested in keeping the parent pointers
    LeafDescentResult descent_result = descend_from_root_to_leaf(
        key,
        false,
        &operation);
    if (descent_result.status == BTreeStatus::EmptyTree) {
        get_result.status = BTreeStatus::KeyNotInTree;
        return get_result;
    }
    if (descent_result.status != BTreeStatus::Success) {
        get_result.status = descent_result.status;
        return get_result;
    }

    // Build the in-memory object for the leaf page
    BLeafPage target_leaf(descent_result.leaf_page);
    
    // Search for the idx of the first key that is greater than or
    // equal to key. If the key doesn't exist or all the values
    // are less than the target key, then return status
    // key not in tree
    std::size_t idx = target_leaf.lower_bound_key(key);
    std::optional<Key> key_at_idx = target_leaf.key_at(idx);
    if (key_at_idx == std::nullopt || !KeyCodec::equal(*key_at_idx, key)) {
        pager->unref_page(descent_result.leaf_page_num);
        get_result.status = BTreeStatus::KeyNotInTree;
        return get_result;
    }

    // Otherwise, we know the key exists. Extract its value
    std::optional<Value> value_opt = target_leaf.get(key);
    Value value = *value_opt;
    get_result.value = value;

    // Ensure to unref the page
    pager->unref_page(descent_result.leaf_page_num);
    return get_result;
}

BTreeStatus BTree::insert(const Key &key, Value &value) {
    return insert_impl(key, value, nullptr, nullptr);
}

BTreeStatus BTree::insert(
    const Key &key,
    Value &value,
    PendingBTreeAction &action
) {
    return insert_impl(key, value, &action, nullptr);
}

BTreeStatus BTree::insert(
    const TransactionHandle &transaction,
    const Key &key,
    Value &value,
    PendingBTreeAction &action
) {
    return insert_impl(key, value, &action, &transaction);
}

BTreeStatus BTree::insert_impl(
    const Key &key,
    Value &value,
    PendingBTreeAction *action,
    const TransactionHandle *transaction,
    Transaction *undo_transaction,
    TransactionUndoExecutor::CompensationAppender *append_compensation
) {
    /**
     * descent <- descend_from_root_to_leaf(key)
     *
     * if descent reports an empty tree {
     *      root_page <- pager.allocate_page()
     *      initialize root_page as a leaf
     *      insert the first key-value pair into it
     *      write it back
     *      pager.set_btree_root(root_page)
     *      pager->unref(root_page)
     *      return success
     * }
     *
     * if descent failed for any other reason, return failure
     *
     * leaf <- BTreeLeafPage(descent.leaf_page.data)
     * idx <- leaf.lower_bound_key(key)
     *
     * if idx is in bounds and leaf.keys[idx] == key {
     *      leaf.values[idx] <- value
     *      leaf.write_back()
     *      pager->unref(descent.leaf_page_num)
     *      return success
     * }
     *
     * leaf.insert_at(idx, key, value)
     * needs_split <- (leaf.key_count() == m)
     *
     * leaf.write_back()
     *
     * if needs_split {
     *      # Recursive helper handles all parent splits and also handles the root case itself
     *      split_result <- propagate_splitting(
     *          split_page_num = descent.leaf_page_num,
     *          path = descent.path
     *      )
     *
     *      pager->unref(descent.leaf_page_num)
     *      return split_result.status
     * }
     *
     * pager->unref(descent.leaf_page_num)
     * return success
    */
    // A split or even an in-place insert can invalidate an active cursor's position.
    if (storage_poisoned) return BTreeStatus::FailedToInsert;
    if (active_cursor_count > 0) return BTreeStatus::CursorActive;

    // Descend from the root to the target leaf.
    // We need the path this time in case the insert changes separators or causes a split.
    BTreeOperation operation(
        transaction && *transaction
            ? (*transaction)->id()
            : (undo_transaction ? undo_transaction->id() : 0),
        pager->page_latch_manager());
    auto finish = [&](BTreeStatus status) {
        if (status != BTreeStatus::Success || !action) return status;
        bool finalized = true;
        if (transaction) {
            finalized = finalize_action(operation, *transaction, *action);
        } else if (append_compensation) {
            finalized = finalize_compensation(operation, *action, *append_compensation);
        }
        if (!finalized) {
            return BTreeStatus::FailedToInsert;
        }
        return BTreeStatus::Success;
    };
    LeafDescentResult descent_result = descend_from_root_to_leaf(
        key,
        true,
        &operation,
        PageLatchMode::Exclusive,
        BTreeMutationType::Insert);
    if (descent_result.status == BTreeStatus::EmptyTree) {
        PagerAllocateResult allocation_result = pager->allocate_page(
            operation,
            V2PageKind::BTreeLeaf);
        if (allocation_result.status != PagerResult::Success) return BTreeStatus::FailedToInsert;
        record_pager_effects(action, std::move(allocation_result.effects));

        std::uint32_t root_page_num = allocation_result.page_num;
        BLeafPage::fill_initial_layout(allocation_result.page->data.data());
        BLeafPage root_leaf(allocation_result.page->data.data());

        bool insert_result = root_leaf.insert_at(0, key, value);
        if (!insert_result) {
            pager->unref_page(root_page_num);
            return BTreeStatus::FailedToInsert;
        }

        root_leaf.write_back();
        record_page_effect(action, PageEffectKind::Write, allocation_result.page);
        PagerMutationResult set_root_result = pager->set_btree_root(
            operation,
            root_page_num);
        pager->unref_page(root_page_num);
        if (set_root_result.status != PagerResult::Success) return BTreeStatus::FailedToInsert;
        record_pager_effects(action, std::move(set_root_result.effects));
        return finish(BTreeStatus::Success);
    }
    if (descent_result.status != BTreeStatus::Success) return descent_result.status;

    // Before mutating the page bytes, tell the pager we are beginning a write on this page.
    PagerResult begin_write_result = pager->begin_write(descent_result.leaf_page_num);
    if (begin_write_result != PagerResult::Success) {
        pager->unref_page(descent_result.leaf_page_num);
        return BTreeStatus::FailedToInsert;
    }

    // Build the in-memory object for the leaf page.
    BLeafPage target_leaf(descent_result.leaf_page);
    std::size_t idx = target_leaf.lower_bound_key(key);
    std::optional<Key> key_at_idx = target_leaf.key_at(idx);

    // If the key already exists, this insert is really just an overwrite of the stored value.
    if (key_at_idx != std::nullopt && KeyCodec::equal(*key_at_idx, key)) {
        bool set_result = target_leaf.set(key, value);
        if (!set_result) {
            pager->unref_page(descent_result.leaf_page_num);
            return BTreeStatus::FailedToInsert;
        }

        target_leaf.write_back();
        record_page_effect(
            action,
            PageEffectKind::Write,
            descent_result.leaf_page_num,
            descent_result.leaf_page);
        pager->unref_page(descent_result.leaf_page_num);
        return finish(BTreeStatus::Success);
    }

    bool insert_result = target_leaf.insert_at(idx, key, value);
    if (!insert_result) {
        pager->unref_page(descent_result.leaf_page_num);
        return BTreeStatus::FailedToInsert;
    }

    bool needs_split = (target_leaf.get_key_count() > MAX_KEYS(BTREE_ORDER));

    // Flush the modified in-memory vectors back into the raw page bytes before handing off
    // to the structural repair helpers.
    target_leaf.write_back();
    record_page_effect(
        action,
        PageEffectKind::Write,
        descent_result.leaf_page_num,
        descent_result.leaf_page);

    if (needs_split) {
        BTreeStatus split_result = propagate_splitting(
            descent_result.leaf_page_num,
            descent_result.path,
            operation,
            action
        );
        pager->unref_page(descent_result.leaf_page_num);
        return finish(split_result);
    }

    pager->unref_page(descent_result.leaf_page_num);
    return finish(BTreeStatus::Success);
}

BTreeRemoveStatus BTree::remove(const Key &key) {
    return remove_impl(key, nullptr, nullptr);
}

BTreeRemoveStatus BTree::remove(
    const Key &key,
    PendingBTreeAction &action
) {
    return remove_impl(key, &action, nullptr);
}

BTreeRemoveStatus BTree::remove(
    const TransactionHandle &transaction,
    const Key &key,
    PendingBTreeAction &action
) {
    return remove_impl(key, &action, &transaction);
}

BTreeRemoveStatus BTree::remove_impl(
    const Key &key,
    PendingBTreeAction *action,
    const TransactionHandle *transaction,
    Transaction *undo_transaction,
    TransactionUndoExecutor::CompensationAppender *append_compensation
) {
    /**
     * descent <- descend_from_root_to_leaf(key)
     * if descent failed, return failure
     *
     * leaf <- BTreeLeafPage(descent.leaf_page.data)
     * idx <- leaf.lower_bound_key(key)
     * if idx is out of bounds or leaf.keys[idx] != key {
     *      pager->unref(descent.leaf_page_num)
     *      return not_found
     * }
     *
     * deleted_was_first_key <- (idx == 0)
     * leaf.remove_at(idx)
     * first_key_changed <- deleted_was_first_key
     * needs_merge_or_borrow <- (leaf.key_count() < minimum_allowed_keys)
     *
     * leaf.write_back()
     *
     * if needs_merge_or_borrow {
     *      merge_result <- propagate_merging(
     *          underflow_page_num = descent.leaf_page_num,
     *          path = descent.path,
     *          underflow_page_type = leaf
     *      )
     *
     *      pager->unref(descent.leaf_page_num)
     *      return merge_result.status
     * }
     *
     * if first_key_changed {
     *      propagate_separator_change_upward(descent.path, leaf.first_key())
     * }
     *
     * pager->unref(descent.leaf_page_num)
     * return success
     */
    BTreeRemoveStatus remove_result{};

    // Deletes can change separator keys, merge pages, or free the cursor's current page.
    if (storage_poisoned) {
        remove_result.status = BTreeStatus::FailedToRemove;
        return remove_result;
    }
    if (active_cursor_count > 0) {
        remove_result.status = BTreeStatus::CursorActive;
        return remove_result;
    }

    BTreeOperation operation(
        transaction && *transaction
            ? (*transaction)->id()
            : (undo_transaction ? undo_transaction->id() : 0),
        pager->page_latch_manager());
    auto finish = [&](BTreeRemoveStatus status) {
        if (status.status != BTreeStatus::Success || !action) return status;
        bool finalized = true;
        if (transaction) {
            finalized = finalize_action(operation, *transaction, *action);
        } else if (append_compensation) {
            finalized = finalize_compensation(operation, *action, *append_compensation);
        }
        if (!finalized) {
            status.status = BTreeStatus::FailedToRemove;
        }
        return status;
    };
    LeafDescentResult descent_result = descend_from_root_to_leaf(
        key,
        true,
        &operation,
        PageLatchMode::Exclusive,
        BTreeMutationType::Remove);
    if (descent_result.status == BTreeStatus::EmptyTree) {
        remove_result.status = BTreeStatus::KeyNotInTree;
        return finish(remove_result);
    }

    if (descent_result.status != BTreeStatus::Success) {
        remove_result.status = descent_result.status;
        return finish(remove_result);
    }

    // Parse the leaf page and find the key
    BLeafPage target_leaf_page(descent_result.leaf_page);
    std::uint32_t leaf_page_num = descent_result.leaf_page_num;
    std::size_t key_idx = target_leaf_page.lower_bound_key(key);
    if (key_idx == target_leaf_page.get_key_count() || !KeyCodec::equal(*target_leaf_page.key_at(key_idx), key)) {
        remove_result.status = BTreeStatus::KeyNotInTree;
        pager->unref_page(leaf_page_num);
        return remove_result;
    }

    // Flag to keep track to whether we removed the first key or not
    bool is_first_key = (key_idx == 0);
    Value value = *target_leaf_page.get_at(key_idx);
    remove_result.value = value;

    // Remove the key
    // Make sure to prep the page for writing first
    PagerResult begin_write_result = pager->begin_write(leaf_page_num);
    if (begin_write_result != PagerResult::Success) {
        remove_result.status = BTreeStatus::FailedToRemove;
        pager->unref_page(leaf_page_num);
        return remove_result;
    }

    bool remove_key_result = target_leaf_page.remove_at(key_idx);
    if (!remove_key_result) {
        remove_result.status = BTreeStatus::FailedToRemove;
        pager->unref_page(leaf_page_num);
        return remove_result;
    }

    // Flush the updated logical leaf state back into the raw page bytes before we
    // start borrowing, merging, or changing separator keys higher up the tree.
    target_leaf_page.write_back();
    record_page_effect(
        action,
        PageEffectKind::Write,
        leaf_page_num,
        descent_result.leaf_page);
    
    // Now check if we went under the min size
    if (target_leaf_page.get_key_count() < MIN_KEYS(BTREE_ORDER)) {
        BTreeStatus merging_status = propagate_merging(
            leaf_page_num,
            descent_result.path,
            operation,
            action);
        pager->unref_page(leaf_page_num);
        remove_result.status = merging_status;
        return finish(remove_result);
    }

    // We are assuming in practice that deleting a key from a non-root leaf node will leave at least one key left in the node
    // That's so we don't have to deal with empty leaf nodes and removing the corresponding separator key

    // If the first key got deleted, propagate it up
    if (is_first_key) {
        BTreeStatus separator_change_status = propagate_separator_change_upward(
            descent_result.path,
            *target_leaf_page.key_at(0),
            operation,
            action);
        pager->unref_page(leaf_page_num);
        remove_result.status = separator_change_status;
        return finish(remove_result);
    }

    pager->unref_page(leaf_page_num);

    return finish(remove_result);
}

void BTree::attach_transaction_manager(
    TransactionManager &manager
) noexcept {
    transaction_manager = &manager;
    if (pager) pager->attach_log(manager.log());
}

bool BTree::finalize_action(
    BTreeOperation &operation,
    const TransactionHandle &transaction,
    PendingBTreeAction &action
) {
    if (!transaction_manager || !transaction) return false;

    // Pending frames cannot be flushed or selected for eviction while the
    // complete multi-page action is waiting for its assigned WAL LSN.
    for (const PageEffect &effect : action.effects()) {
        PagerResult pending_result = pager->mark_wal_pending(
            operation,
            effect.page_num);
        if (pending_result != PagerResult::Success) {
            storage_poisoned = true;
            return false;
        }
    }

    Lsn lsn = 0;
    try {
        // Append the complete action before publishing its LSN into any page.
        // The operation still owns every exclusive logical latch at this point.
        lsn = transaction_manager->append_action(
            transaction,
            action.build());
    } catch (...) {
        storage_poisoned = true;
        throw;
    }

    // Reload each affected frame by logical page number, install the assigned
    // LSN, and recompute its checksum before allowing the latch to be released.
    for (const PageEffect &effect : action.effects()) {
        PagerResult install_result = pager->install_page_lsn(
            operation,
            effect.page_num,
            lsn);
        if (install_result != PagerResult::Success) {
            storage_poisoned = true;
            return false;
        }
    }
    return true;
}

bool BTree::finalize_compensation(
    BTreeOperation &operation,
    PendingBTreeAction &action,
    TransactionUndoExecutor::CompensationAppender &append_compensation
) {
    for (const PageEffect &effect : action.effects()) {
        if (pager->mark_wal_pending(operation, effect.page_num) != PagerResult::Success) {
            storage_poisoned = true;
            return false;
        }
    }

    Lsn lsn = 0;
    try {
        lsn = append_compensation(action.effects());
    } catch (...) {
        storage_poisoned = true;
        throw;
    }

    for (const PageEffect &effect : action.effects()) {
        if (pager->install_page_lsn(operation, effect.page_num, lsn) != PagerResult::Success) {
            storage_poisoned = true;
            return false;
        }
    }
    return true;
}

bool BTree::apply_undo(
    Transaction &transaction,
    const UndoDescriptor &undo,
    TransactionUndoExecutor::CompensationAppender append_compensation
) {
    PendingBTreeAction action(transaction.id(), transaction.last_lsn());
    action.set_undo(undo);

    if (const auto *insert = std::get_if<InsertUndo>(&undo)) {
        return remove_impl(
            insert->key,
            &action,
            nullptr,
            &transaction,
            &append_compensation).status == BTreeStatus::Success;
    }

    Key key{};
    Value value{};
    if (const auto *update = std::get_if<UpdateUndo>(&undo)) {
        key = update->key;
        value = update->old_value;
    } else {
        const auto &deletion = std::get<DeleteUndo>(undo);
        key = deletion.key;
        value = deletion.old_value;
    }
    return insert_impl(
        key,
        value,
        &action,
        nullptr,
        &transaction,
        &append_compensation) == BTreeStatus::Success;
}



LeafDescentResult BTree::descend_from_root_to_leaf(
    const Key &key,
    bool include_path,
    BTreeOperation *operation,
    PageLatchMode latch_mode,
    BTreeMutationType mutation_type
) {
    LeafDescentResult descent_result{};

    // Holding page zero keeps the root page number stable until the root's
    // own latch has been acquired.
    if (operation) {
        if (latch_mode == PageLatchMode::Shared) {
            operation->lock_shared(0);
        } else {
            operation->lock_exclusive(0);
        }
    }
    PagerGetRootResult root_get_result = pager->get_btree_root();

    if (root_get_result.status == PagerResult::EmptyBTree) {
        // The first inserter must retain page zero until it installs the root.
        // Releasing and reacquiring it would let another inserter observe the
        // same empty tree and create a competing root page.
        if (
            operation &&
            !(
                latch_mode == PageLatchMode::Exclusive &&
                mutation_type == BTreeMutationType::Insert
            )
        ) {
            operation->release(0);
        }
        descent_result.status = BTreeStatus::EmptyTree;
        return descent_result;
    }
    if (root_get_result.status != PagerResult::Success) {
        if (operation) operation->release(0);
        descent_result.status = BTreeStatus::FailedToGetRoot;
        return descent_result;
    }

    // Acquire the root before releasing page zero. The root frame remains
    // referenced independently by Pager until this descent releases it.
    if (operation) {
        if (latch_mode == PageLatchMode::Shared) {
            operation->lock_shared(root_get_result.root_page_num);
            operation->release(0);
        } else {
            operation->lock_exclusive(root_get_result.root_page_num);
        }
    }

    // If the root is already a leaf, return the root
    PageType root_page_type = BTreePage::peek_page_type(root_get_result.page->data.data());
    if (root_page_type == PageType::Leaf) {
        // A root leaf that can absorb this insert cannot split, so page zero
        // cannot change and no longer needs to remain exclusively latched.
        if (
            operation &&
            latch_mode == PageLatchMode::Exclusive &&
            mutation_type == BTreeMutationType::Insert
        ) {
            BLeafPage root_page(root_get_result.page->data.data());
            if (root_page.get_key_count() < MAX_KEYS(BTREE_ORDER)) {
                operation->release_all_exclusive_except(
                    root_get_result.root_page_num);
            }
        } else if (
            operation &&
            latch_mode == PageLatchMode::Exclusive &&
            mutation_type == BTreeMutationType::Remove
        ) {
            BLeafPage root_page(root_get_result.page->data.data());
            const std::size_t key_idx = root_page.lower_bound_key(key);
            const std::optional<Key> existing_key = root_page.key_at(key_idx);
            const bool key_exists =
                existing_key && KeyCodec::equal(*existing_key, key);

            // A missing key makes no change. An existing key is safe when the
            // root leaf will remain nonempty and page zero cannot change.
            if (!key_exists || root_page.get_key_count() > 1) {
                operation->release_all_exclusive_except(
                    root_get_result.root_page_num);
            }
        }
        descent_result.leaf_page = root_get_result.page->data.data();
        descent_result.leaf_page_num = root_get_result.root_page_num;
        return descent_result;
    }

    // Otherwise, keep on descending downwards until we hit a leaf
    std::uint32_t curr_page_num = root_get_result.root_page_num;
    char *curr_data = root_get_result.page->data.data();
    while (true) {
        // Decode the current node raw bytes into an in-memory internal page object
        BInternalPage curr(curr_data);
        std::size_t idx = curr.lower_bound_key(key); // The idx of the first key greater or equal to the key

        std::optional<std::uint32_t> target_child_page_num;
        ChildDirection child_dir;
        if (idx == curr.get_key_count()) {
            // The target key is greater than all keys in the current node
            target_child_page_num = curr.get_right_child(idx - 1);
            child_dir = ChildDirection::Right;
        } else if (KeyCodec::equal(*curr.key_at(idx), key)) {
            // The key at the idx is equal to the target
            target_child_page_num = curr.get_right_child(idx);
            child_dir = ChildDirection::Right;
        } else {
            // The key at the idx is greater than the target
            target_child_page_num = curr.get_left_child(idx);
            child_dir = ChildDirection::Left;
        }

        // Latch the child before releasing the parent so structural changes
        // cannot invalidate the route between these two pages.
        if (operation) {
            if (latch_mode == PageLatchMode::Shared) {
                operation->lock_shared(*target_child_page_num);
            } else {
                operation->lock_exclusive(*target_child_page_num);
            }
        }

        // Get the computed child page number. That's the next page in our traversal
        PagerGetResult pager_get_result = pager->get(*target_child_page_num);

        // Handle failure of reading before inspecting the child frame.
        if (pager_get_result.status != PagerResult::Success) {
            pager->unref_page(curr_page_num);
            if (operation && latch_mode == PageLatchMode::Shared) {
                operation->release(curr_page_num);
            }
            if (operation) operation->release(*target_child_page_num);
            descent_result.status = BTreeStatus::FailedToRead;
            return descent_result;
        }

        PageType child_page_type = BTreePage::peek_page_type(
            pager_get_result.page->data.data());

        // At a leaf we can prove that this insert cannot split anywhere, so
        // neither tree ancestors nor page-zero allocation metadata are needed.
        if (
            operation &&
            latch_mode == PageLatchMode::Exclusive &&
            mutation_type == BTreeMutationType::Insert &&
            child_page_type == PageType::Leaf
        ) {
            BLeafPage child_page(pager_get_result.page->data.data());
            if (child_page.get_key_count() < MAX_KEYS(BTREE_ORDER)) {
                operation->release_all_exclusive_except(
                    *target_child_page_num);
            }
        }

        // A leaf deletion is safe only when it cannot underflow and cannot
        // change the separator that represents the leaf in its parent.
        if (
            operation &&
            latch_mode == PageLatchMode::Exclusive &&
            mutation_type == BTreeMutationType::Remove &&
            child_page_type == PageType::Leaf
        ) {
            BLeafPage child_page(pager_get_result.page->data.data());
            const std::size_t key_idx = child_page.lower_bound_key(key);
            const std::optional<Key> existing_key = child_page.key_at(key_idx);
            const bool key_exists =
                existing_key && KeyCodec::equal(*existing_key, key);
            const bool child_is_safe =
                !key_exists ||
                (
                    key_idx != 0 &&
                    child_page.get_key_count() > MIN_KEYS(BTREE_ORDER)
                );

            if (child_is_safe) {
                operation->release_all_exclusive_except(
                    *target_child_page_num);
            }
        }

        // The logical latch may be retained for propagation even though the
        // cache frame itself is no longer needed during downward traversal.
        pager->unref_page(curr_page_num);
        if (operation && latch_mode == PageLatchMode::Shared) {
            operation->release(curr_page_num);
        }

        // If the caller wants us to record our path to the target leaf, record the page num 
        // of the current internal node + the key idx we took
        if (include_path) {
            std::size_t pivot_idx = ((idx == curr.get_key_count()) ? idx - 1 : idx);
            TraversalPathEntry entry{curr_page_num, pivot_idx, child_dir};
            descent_result.path.push_back(entry);
        }

        // Set the new curr values
        curr_page_num = *target_child_page_num;
        curr_data = pager_get_result.page->data.data();

        // If the next page is a leaf, terminate the search here
        if (child_page_type == PageType::Leaf) break;
    }

    // Finalize result
    descent_result.leaf_page = curr_data;
    descent_result.leaf_page_num = curr_page_num;
    return descent_result;
}

BTreeStatus BTree::propagate_splitting(
    std::uint32_t split_page_num,
    std::vector<TraversalPathEntry> &path,
    BTreeOperation &operation,
    PendingBTreeAction *action
) {
    /**
     * The page split_page_num overflowed after an insert.
     * Split it into the old left page plus a new right sibling, then repair the parent.
     * If the parent overflows too, recurse upward.
     *
     * Leaf split:
     * - move the upper half of the key-value pairs into a new right leaf
     * - promote the first key of that new right leaf
     * - that promoted key stays in the right leaf
     *
     * Internal split:
     * - split around the median separator
     * - move the separators to the right of the median, plus their children, into a new right internal page
     * - promote the median key upward
     * - that promoted key does not stay in either child
     *
     * Parent repair:
     * - if there is a parent, insert the promoted key and the new right child
     *   immediately to the right of the old split page
     * - if there is no parent, the split page was the root, so allocate a new internal root
     *   and update the pager's root page number
     *
     * Insert splits do not readjust any older separator keys.
     * The only new separator here is the one produced by the split itself.
     */
    PagerGetResult get_split_page_result = pager->get(split_page_num);
    if (get_split_page_result.status != PagerResult::Success) {
        pager->unref_page(split_page_num);
        return BTreeStatus::FailedToRead;
    }

    PageType page_type = BTreePage::peek_page_type(get_split_page_result.page->data.data());
    pager->begin_write(split_page_num);
    if (page_type == PageType::Leaf) {
        BLeafPage split_page(get_split_page_result.page->data.data());

        PagerAllocateResult allocation_result = pager->allocate_page(
            operation,
            V2PageKind::BTreeLeaf);
        if (allocation_result.status != PagerResult::Success) {
            pager->unref_page(split_page_num);
            return BTreeStatus::FailedToAllocateNewPage;
        }
        record_pager_effects(action, std::move(allocation_result.effects));
        
        std::uint32_t new_leaf_page_num = allocation_result.page_num;
        BLeafPage::fill_initial_layout(allocation_result.page->data.data());
        BLeafPage new_leaf_page(allocation_result.page->data.data());

        std::size_t median_separator_idx = split_page.get_key_count() / 2;
        BTree::migrate_leaf(split_page, new_leaf_page, median_separator_idx); // Assumes it does the write_back on both
        record_page_effect(
            action,
            PageEffectKind::Write,
            split_page_num,
            get_split_page_result.page->data.data());
        record_page_effect(action, PageEffectKind::Write, allocation_result.page);

        if (path.size() == 0) {
            // The current node we are splitting is a root node
            // Handle the splitting of the root and return
            BTreeStatus split_root = handle_splitting_root(
                split_page_num,
                new_leaf_page_num,
                *new_leaf_page.key_at(0),
                operation,
                action);
            pager->unref_page(split_page_num);
            pager->unref_page(new_leaf_page_num);
            return split_root;
        }

        TraversalPathEntry parent_path_entry = path.back();
        path.pop_back();

        // Get the parent page
        PagerGetResult get_parent_result = pager->get(parent_path_entry.parent_page_num);
        if (get_parent_result.status != PagerResult::Success) {
            pager->unref_page(split_page_num);
            pager->unref_page(new_leaf_page_num);
            return BTreeStatus::FailedToRead;
        }
        pager->begin_write(parent_path_entry.parent_page_num);

        // The parent page is surely definitely an internal page
        // Insert the new separator key at the idx after the parent separator_index_used
        BInternalPage parent_page(get_parent_result.page->data.data());
        Key key = *new_leaf_page.key_at(0);
        bool insert_separator_result = false;
        if (parent_path_entry.child_dir == ChildDirection::Right) {
            insert_separator_result = parent_page.insert_separator_at(parent_path_entry.separator_index_used + 1, key, new_leaf_page_num);
        } else {
            // This also handles the case of inserting at the start of the page. The leftmost child remains at the same page number
            insert_separator_result = parent_page.insert_separator_at(parent_path_entry.separator_index_used, key, new_leaf_page_num);
        }
        if (!insert_separator_result) {
            pager->unref_page(parent_path_entry.parent_page_num);
            pager->unref_page(split_page_num);
            pager->unref_page(new_leaf_page_num);
            return BTreeStatus::FailedToInsert;
        }

        // Leaf splits need no key adjustments on the separators. Ignored
        // Now check if we need to split the parent
        bool parent_needs_split = (parent_page.get_key_count() > MAX_KEYS(BTREE_ORDER));
        parent_page.write_back();
        record_page_effect(action, PageEffectKind::Write, get_parent_result.page);

        pager->unref_page(split_page_num);
        pager->unref_page(new_leaf_page_num);

        if (parent_needs_split) {
            pager->unref_page(parent_path_entry.parent_page_num);
            return propagate_splitting(
                parent_path_entry.parent_page_num,
                path,
                operation,
                action);
        }

        pager->unref_page(parent_path_entry.parent_page_num);
        return BTreeStatus::Success;
    } else {
        BInternalPage split_page(get_split_page_result.page->data.data());

        // Allocate the new internal page that is going to hold everything to the right
        // of the promoted median separator.
        PagerAllocateResult allocation_result = pager->allocate_page(
            operation,
            V2PageKind::BTreeInternal);
        if (allocation_result.status != PagerResult::Success) {
            pager->unref_page(split_page_num);
            return BTreeStatus::FailedToAllocateNewPage;
        }
        record_pager_effects(action, std::move(allocation_result.effects));

        std::uint32_t new_internal_page_num = allocation_result.page_num;
        BInternalPage::fill_initial_layout(allocation_result.page->data.data());
        BInternalPage new_internal_page(allocation_result.page->data.data());

        // Capture the median key before mutating the original page.
        // This is the key that gets promoted upward and does not stay in either child page.
        std::size_t median_separator_idx = split_page.get_key_count() / 2;
        std::optional<Key> promoted_key = split_page.key_at(median_separator_idx);
        if (promoted_key == std::nullopt) {
            pager->unref_page(split_page_num);
            pager->unref_page(new_internal_page_num);
            return BTreeStatus::FailedToInsert;
        }

        // Move everything strictly to the right of the promoted median into the new internal page.
        // The helper also installs the new page's leftmost child and removes the promoted median
        // from the old page.
        BTree::migrate_internal(split_page, new_internal_page, median_separator_idx);

        split_page.write_back();
        new_internal_page.write_back();
        record_page_effect(action, PageEffectKind::Write, get_split_page_result.page);
        record_page_effect(action, PageEffectKind::Write, allocation_result.page);

        if (path.size() == 0) {
            // The current internal page being split is the root.
            // Create a new root above the two split children.
            BTreeStatus split_root = handle_splitting_root(
                split_page_num,
                new_internal_page_num,
                *promoted_key,
                operation,
                action);
            pager->unref_page(split_page_num);
            pager->unref_page(new_internal_page_num);
            return split_root;
        }

        TraversalPathEntry parent_path_entry = path.back();
        path.pop_back();

        // Load the parent internal page and insert the promoted key together with
        // the new right internal child page number.
        PagerGetResult get_parent_result = pager->get(parent_path_entry.parent_page_num);
        if (get_parent_result.status != PagerResult::Success) {
            pager->unref_page(split_page_num);
            pager->unref_page(new_internal_page_num);
            return BTreeStatus::FailedToRead;
        }
        pager->begin_write(parent_path_entry.parent_page_num);

        BInternalPage parent_page(get_parent_result.page->data.data());
        bool insert_separator_result = false;
        if (parent_path_entry.child_dir == ChildDirection::Right) {
            insert_separator_result = parent_page.insert_separator_at(
                parent_path_entry.separator_index_used + 1,
                *promoted_key,
                new_internal_page_num
            );
        } else {
            insert_separator_result = parent_page.insert_separator_at(
                parent_path_entry.separator_index_used,
                *promoted_key,
                new_internal_page_num
            );
        }
        if (!insert_separator_result) {
            pager->unref_page(parent_path_entry.parent_page_num);
            pager->unref_page(split_page_num);
            pager->unref_page(new_internal_page_num);
            return BTreeStatus::FailedToInsert;
        }

        // Internal-page splits also need no readjustment of existing parent separators.
        // The promoted median already becomes the new separator for the new right child.
        bool parent_needs_split = (parent_page.get_key_count() > MAX_KEYS(BTREE_ORDER));
        parent_page.write_back();
        record_page_effect(action, PageEffectKind::Write, get_parent_result.page);

        pager->unref_page(split_page_num);
        pager->unref_page(new_internal_page_num);

        if (parent_needs_split) {
            pager->unref_page(parent_path_entry.parent_page_num);
            return propagate_splitting(
                parent_path_entry.parent_page_num,
                path,
                operation,
                action);
        }

        pager->unref_page(parent_path_entry.parent_page_num);
        return BTreeStatus::Success;
    }

    return BTreeStatus::FailedToInsert;
}

void BTree::migrate_leaf(BLeafPage &src, BLeafPage &dst, std::size_t separator_idx) {
    // Move every key-value pair starting at the split point into the new right leaf.
    // We keep removing from the same logical index because the left leaf shrinks after each move.
    while (src.get_key_count() > separator_idx) {
        std::optional<Key> move_key = src.key_at(separator_idx);
        if (move_key == std::nullopt) std::abort();

        std::optional<Value> move_value = src.get(*move_key);
        if (move_value == std::nullopt) std::abort();

        bool insert_result = dst.insert_at(dst.get_key_count(), *move_key, *move_value);
        if (!insert_result) std::abort();

        bool remove_result = src.remove_at(separator_idx);
        if (!remove_result) std::abort();
    }

    // Flush both in-memory page objects back into their raw page bytes before the caller
    // starts repairing parent pointers and separators.
    src.write_back();
    dst.write_back();
}

void BTree::migrate_internal(BInternalPage &src, BInternalPage &dst, std::size_t median_separator_idx) {
    // The child immediately to the right of the promoted median becomes the leftmost
    // child of the new internal page.
    std::optional<std::uint32_t> new_leftmost_child = src.get_right_child(median_separator_idx);
    if (new_leftmost_child == std::nullopt) std::abort();

    bool set_leftmost_result = dst.set_leftmost_child(*new_leftmost_child);
    if (!set_leftmost_result) std::abort();

    // Move every separator strictly to the right of the median into the new right page.
    // We keep removing from the same logical index because the vector shrinks after each removal.
    while (src.get_key_count() > median_separator_idx + 1) {
        std::optional<Key> move_key = src.key_at(median_separator_idx + 1);
        std::optional<std::uint32_t> move_right_child = src.get_right_child(median_separator_idx + 1);
        if (move_key == std::nullopt || move_right_child == std::nullopt) std::abort();

        bool insert_separator_result = dst.insert_separator_at(
            dst.get_key_count(),
            *move_key,
            *move_right_child
        );
        if (!insert_separator_result) std::abort();

        bool remove_separator_result = src.remove_separator_at(median_separator_idx + 1);
        if (!remove_separator_result) std::abort();
    }

    // Finally remove the promoted median from the original page.
    // Its right child already became the leftmost child of the new page.
    bool remove_promoted_result = src.remove_separator_at(median_separator_idx);
    if (!remove_promoted_result) std::abort();
}

BTreeStatus BTree::handle_splitting_root(
    std::uint32_t left_child_page_num,
    std::uint32_t right_child_page_num,
    const Key &separator_key,
    BTreeOperation &operation,
    PendingBTreeAction *action
) {
    // A root split means we need to allocate a brand new internal root above the
    // two children that just came out of the split.
    PagerAllocateResult allocation_result = pager->allocate_page(
        operation,
        V2PageKind::BTreeInternal);
    if (allocation_result.status != PagerResult::Success) return BTreeStatus::FailedToAllocateNewPage;
    record_pager_effects(action, std::move(allocation_result.effects));

    std::uint32_t new_root_page_num = allocation_result.page_num;
    BInternalPage::fill_initial_layout(allocation_result.page->data.data());
    BInternalPage new_root_page(allocation_result.page->data.data());

    // The old root becomes the leftmost child, and the split-off page becomes the child
    // to the right of the one separator key stored in the new root.
    // fill_initial_layout writes a placeholder leftmost child into the raw page bytes.
    // So once we decode this page object, the leftmost child slot already exists and
    // we need to replace it instead of trying to append a brand new one.
    bool set_leftmost_result = new_root_page.replace_leftmost_child(left_child_page_num);
    if (!set_leftmost_result) {
        pager->unref_page(new_root_page_num);
        return BTreeStatus::FailedToInsert;
    }

    bool insert_separator_result = new_root_page.insert_separator_at(0, separator_key, right_child_page_num);
    if (!insert_separator_result) {
        pager->unref_page(new_root_page_num);
        return BTreeStatus::FailedToInsert;
    }

    new_root_page.write_back();
    record_page_effect(action, PageEffectKind::Write, allocation_result.page);

    PagerMutationResult set_root_result = pager->set_btree_root(
        operation,
        new_root_page_num);
    pager->unref_page(new_root_page_num);
    if (set_root_result.status != PagerResult::Success) return BTreeStatus::FailedToInsert;
    record_pager_effects(action, std::move(set_root_result.effects));

    return BTreeStatus::Success;
}

BTreeStatus BTree::propagate_separator_change_upward(
    const std::vector<TraversalPathEntry> &path,
    const Key &new_subtree_min,
    BTreeOperation &operation,
    PendingBTreeAction *action
) {
    /**
     * A delete changed the minimum key of some subtree.
     *
     * Walk upward from the parent of that subtree:
     * - if the subtree is not the leftmost child at this level, update the one
     *   separator key that names it and stop
     * - if the subtree is the leftmost child, this parent stores nothing for it,
     *   so keep climbing
     *
     * If we stay on the leftmost spine all the way to the root, then no separator
     * anywhere stores this minimum and there is nothing to update.
     */
    for (std::size_t i = path.size(); i > 0; i--) {
        const TraversalPathEntry &entry = path[i - 1];

        // If we reached this subtree by taking the right side of a separator, then
        // that separator stores this subtree's minimum. Update it and stop.
        if (entry.child_dir == ChildDirection::Right) {
            operation.lock_exclusive(entry.parent_page_num);
            PagerGetResult get_parent_result = pager->get(entry.parent_page_num);
            if (get_parent_result.status != PagerResult::Success) return BTreeStatus::FailedToRead;

            PagerResult begin_write_result = pager->begin_write(entry.parent_page_num);
            if (begin_write_result != PagerResult::Success) {
                pager->unref_page(entry.parent_page_num);
                return BTreeStatus::FailedToRemove;
            }

            BInternalPage parent_page(get_parent_result.page->data.data());
            bool set_result = parent_page.set_separator_key_at(entry.separator_index_used, new_subtree_min);
            if (!set_result) {
                pager->unref_page(entry.parent_page_num);
                return BTreeStatus::FailedToRemove;
            }

            parent_page.write_back();
            record_page_effect(action, PageEffectKind::Write, get_parent_result.page);
            pager->unref_page(entry.parent_page_num);
            return BTreeStatus::Success;
        }

        // If we took the left side of separator 0, then this subtree is still the
        // leftmost child at this level. There is nothing to update here, so keep
        // walking upward.
        if (entry.separator_index_used == 0) continue;

        // Otherwise we took the left side of some separator strictly after the first.
        // That means this subtree is not the leftmost child overall, and the separator
        // immediately before it stores its minimum.
        operation.lock_exclusive(entry.parent_page_num);
        PagerGetResult get_parent_result = pager->get(entry.parent_page_num);
        if (get_parent_result.status != PagerResult::Success) return BTreeStatus::FailedToRead;

        PagerResult begin_write_result = pager->begin_write(entry.parent_page_num);
        if (begin_write_result != PagerResult::Success) {
            pager->unref_page(entry.parent_page_num);
            return BTreeStatus::FailedToRemove;
        }

        BInternalPage parent_page(get_parent_result.page->data.data());
        bool set_result = parent_page.set_separator_key_at(entry.separator_index_used - 1, new_subtree_min);
        if (!set_result) {
            pager->unref_page(entry.parent_page_num);
            return BTreeStatus::FailedToRemove;
        }

        parent_page.write_back();
        record_page_effect(action, PageEffectKind::Write, get_parent_result.page);
        pager->unref_page(entry.parent_page_num);
        return BTreeStatus::Success;
    }

    return BTreeStatus::Success;
}
BTreeStatus BTree::handle_root_underflow(
    std::uint32_t underflow_page_num,
    PageType underflow_page_type,
    char *underflow_page_data,
    BTreeOperation &operation,
    PendingBTreeAction *action
) {
    // The root is special. It is allowed to violate the usual min-size rules.
    if (underflow_page_type == PageType::Leaf) {
        BLeafPage root_leaf(underflow_page_data);

        // If this leaf root still has data in it, nothing to repair.
        if (root_leaf.get_key_count() > 0) {
            pager->unref_page(underflow_page_num);
            return BTreeStatus::Success;
        }

        // The tree became empty. Clear the root pointer in the header and free this page.
        PagerMutationResult clear_root_result = pager->set_btree_root(operation, 0);
        if (clear_root_result.status != PagerResult::Success) {
            pager->unref_page(underflow_page_num);
            return BTreeStatus::FailedToRemove;
        }
        record_pager_effects(action, std::move(clear_root_result.effects));

        PagerMutationResult free_root_result = pager->free_page(
            operation,
            underflow_page_num);
        pager->unref_page(underflow_page_num);
        if (free_root_result.status != PagerResult::Success) return BTreeStatus::FailedToRemove;
        record_pager_effects(action, std::move(free_root_result.effects));
        return BTreeStatus::Success;
    }

    BInternalPage root_page(underflow_page_data);

    // Non-empty internal roots are fine. Root nodes can stay below the usual minimum.
    if (root_page.get_key_count() > 0) {
        pager->unref_page(underflow_page_num);
        return BTreeStatus::Success;
    }

    // A 0-key internal root must collapse into its only child.
    std::optional<std::uint32_t> only_child_page_num = root_page.get_leftmost_child();
    if (only_child_page_num == std::nullopt) {
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }

    PagerMutationResult set_root_result = pager->set_btree_root(
        operation,
        *only_child_page_num);
    if (set_root_result.status != PagerResult::Success) {
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }
    record_pager_effects(action, std::move(set_root_result.effects));

    PagerMutationResult free_root_result = pager->free_page(
        operation,
        underflow_page_num);
    pager->unref_page(underflow_page_num);
    if (free_root_result.status != PagerResult::Success) return BTreeStatus::FailedToRemove;
    record_pager_effects(action, std::move(free_root_result.effects));
    return BTreeStatus::Success;
}

BTree::MergeParentContext BTree::build_merge_parent_context(const TraversalPathEntry &parent_path_entry, BInternalPage &parent_page) {
    MergeParentContext ctx{};

    // Translate the path metadata into the actual child idx inside the parent page.
    // Once we have that idx, sibling lookup becomes straightforward.
    ctx.parent_page_num = parent_path_entry.parent_page_num;
    ctx.child_idx = parent_path_entry.separator_index_used;
    if (parent_path_entry.child_dir == ChildDirection::Right) ctx.child_idx += 1;

    // The right sibling sits to the right of separator child_idx if that separator exists.
    if (ctx.child_idx < parent_page.get_key_count()) {
        ctx.right_sibling_page_num = parent_page.get_right_child(ctx.child_idx);
    }

    // The left sibling is either the parent's leftmost child or the child to the right
    // of the separator immediately before us.
    if (ctx.child_idx > 0) {
        if (ctx.child_idx == 1) {
            ctx.left_sibling_page_num = parent_page.get_left_child(0);
        } else {
            ctx.left_sibling_page_num = parent_page.get_right_child(ctx.child_idx - 2);
        }
    }

    return ctx;
}

BTreeStatus BTree::finish_parent_after_merge(
    BInternalPage &parent_page,
    std::uint32_t parent_page_num,
    std::vector<TraversalPathEntry> &path,
    BTreeOperation &operation,
    PendingBTreeAction *action
) {
    // Merging deleted one separator from the parent. If that pushed the parent below
    // the minimum, keep repairing upward recursively.
    bool parent_underflow = parent_page.get_key_count() < MIN_KEYS(BTREE_ORDER);
    path.pop_back();
    pager->unref_page(parent_page_num);
    if (parent_underflow) return propagate_merging(
        parent_page_num,
        path,
        operation,
        action);
    return BTreeStatus::Success;
}

BTreeStatus BTree::borrow_from_right_leaf(
    BLeafPage &current_leaf,
    BInternalPage &parent_page,
    const MergeParentContext &ctx,
    std::uint32_t underflow_page_num,
    PendingBTreeAction *action
) {
    if (ctx.right_sibling_page_num == std::nullopt) return BTreeStatus::FailedToRemove;

    // Load the right sibling and first make sure it can actually spare a key.
    PagerGetResult get_right_result = pager->get(static_cast<int>(*ctx.right_sibling_page_num));
    if (get_right_result.status != PagerResult::Success) {
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRead;
    }

    BLeafPage right_sibling(get_right_result.page->data.data());
    if (right_sibling.get_key_count() <= MIN_KEYS(BTREE_ORDER)) {
        pager->unref_page(static_cast<int>(*ctx.right_sibling_page_num));
        return BTreeStatus::FailedToRemove;
    }

    PagerResult begin_write_right_result = pager->begin_write(static_cast<int>(*ctx.right_sibling_page_num));
    if (begin_write_right_result != PagerResult::Success) {
        pager->unref_page(static_cast<int>(*ctx.right_sibling_page_num));
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }

    // Borrow the first record from the right sibling and append it to the current leaf.
    // Then update the parent separator so it names the new minimum of the right subtree.
    std::optional<Key> borrowed_key = right_sibling.key_at(0);
    std::optional<Value> borrowed_value = right_sibling.get_at(0);
    if (borrowed_key == std::nullopt || borrowed_value == std::nullopt) {
        pager->unref_page(static_cast<int>(*ctx.right_sibling_page_num));
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }

    bool insert_result = current_leaf.insert_at(current_leaf.get_key_count(), *borrowed_key, *borrowed_value);
    bool remove_result = right_sibling.remove_at(0);
    std::optional<Key> new_right_first_key = right_sibling.first_key();
    bool set_sep_result = (new_right_first_key != std::nullopt) &&
        parent_page.set_separator_key_at(ctx.child_idx, *new_right_first_key);
    if (!insert_result || !remove_result || !set_sep_result) {
        pager->unref_page(static_cast<int>(*ctx.right_sibling_page_num));
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }

    current_leaf.write_back();
    right_sibling.write_back();
    parent_page.write_back();
    record_page_effect(action, PageEffectKind::Write, underflow_page_num, current_leaf.data());
    record_page_effect(action, PageEffectKind::Write, *ctx.right_sibling_page_num, right_sibling.data());
    record_page_effect(action, PageEffectKind::Write, ctx.parent_page_num, parent_page.data());

    pager->unref_page(static_cast<int>(*ctx.right_sibling_page_num));
    pager->unref_page(ctx.parent_page_num);
    pager->unref_page(underflow_page_num);
    return BTreeStatus::Success;
}

BTreeStatus BTree::borrow_from_left_leaf(
    BLeafPage &current_leaf,
    BInternalPage &parent_page,
    const MergeParentContext &ctx,
    std::uint32_t underflow_page_num,
    PendingBTreeAction *action
) {
    if (ctx.left_sibling_page_num == std::nullopt) return BTreeStatus::FailedToRemove;

    // Load the left sibling and make sure it can actually lend.
    PagerGetResult get_left_result = pager->get(static_cast<int>(*ctx.left_sibling_page_num));
    if (get_left_result.status != PagerResult::Success) {
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRead;
    }

    BLeafPage left_sibling(get_left_result.page->data.data());
    if (left_sibling.get_key_count() <= MIN_KEYS(BTREE_ORDER)) {
        pager->unref_page(static_cast<int>(*ctx.left_sibling_page_num));
        return BTreeStatus::FailedToRemove;
    }

    PagerResult begin_write_left_result = pager->begin_write(static_cast<int>(*ctx.left_sibling_page_num));
    if (begin_write_left_result != PagerResult::Success) {
        pager->unref_page(static_cast<int>(*ctx.left_sibling_page_num));
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }

    // Borrow the last record from the left sibling and prepend it to the current leaf.
    // The current leaf's minimum changed, so we must update the separator that names it.
    std::size_t left_last_idx = left_sibling.get_key_count() - 1;
    std::optional<Key> borrowed_key = left_sibling.key_at(left_last_idx);
    std::optional<Value> borrowed_value = left_sibling.get_at(left_last_idx);
    if (borrowed_key == std::nullopt || borrowed_value == std::nullopt) {
        pager->unref_page(static_cast<int>(*ctx.left_sibling_page_num));
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }

    bool remove_result = left_sibling.remove_at(left_last_idx);
    bool insert_result = current_leaf.insert_at(0, *borrowed_key, *borrowed_value);
    std::optional<Key> new_current_first_key = current_leaf.first_key();
    bool set_sep_result = (new_current_first_key != std::nullopt) &&
        parent_page.set_separator_key_at(ctx.child_idx - 1, *new_current_first_key);
    if (!remove_result || !insert_result || !set_sep_result) {
        pager->unref_page(static_cast<int>(*ctx.left_sibling_page_num));
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }

    left_sibling.write_back();
    current_leaf.write_back();
    parent_page.write_back();
    record_page_effect(action, PageEffectKind::Write, *ctx.left_sibling_page_num, left_sibling.data());
    record_page_effect(action, PageEffectKind::Write, underflow_page_num, current_leaf.data());
    record_page_effect(action, PageEffectKind::Write, ctx.parent_page_num, parent_page.data());

    pager->unref_page(static_cast<int>(*ctx.left_sibling_page_num));
    pager->unref_page(ctx.parent_page_num);
    pager->unref_page(underflow_page_num);
    return BTreeStatus::Success;
}

BTreeStatus BTree::merge_with_right_leaf(
    BLeafPage &current_leaf,
    BInternalPage &parent_page,
    const MergeParentContext &ctx,
    std::uint32_t underflow_page_num,
    std::vector<TraversalPathEntry> &path,
    BTreeOperation &operation,
    PendingBTreeAction *action
) {
    if (ctx.right_sibling_page_num == std::nullopt) return BTreeStatus::FailedToRemove;

    // Merge the right sibling into the current leaf.
    // The current leaf survives, the right sibling page gets freed, and the parent
    // loses the separator that used to name that right sibling.
    PagerGetResult get_right_result = pager->get(static_cast<int>(*ctx.right_sibling_page_num));
    if (get_right_result.status != PagerResult::Success) {
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRead;
    }

    BLeafPage right_sibling(get_right_result.page->data.data());
    while (right_sibling.get_key_count() > 0) {
        std::optional<Key> move_key = right_sibling.key_at(0);
        std::optional<Value> move_value = right_sibling.get_at(0);
        if (move_key == std::nullopt || move_value == std::nullopt) {
            pager->unref_page(static_cast<int>(*ctx.right_sibling_page_num));
            pager->unref_page(ctx.parent_page_num);
            pager->unref_page(underflow_page_num);
            return BTreeStatus::FailedToRemove;
        }

        bool insert_result = current_leaf.insert_at(current_leaf.get_key_count(), *move_key, *move_value);
        bool remove_result = right_sibling.remove_at(0);
        if (!insert_result || !remove_result) {
            pager->unref_page(static_cast<int>(*ctx.right_sibling_page_num));
            pager->unref_page(ctx.parent_page_num);
            pager->unref_page(underflow_page_num);
            return BTreeStatus::FailedToRemove;
        }
    }

    bool remove_parent_sep_result = parent_page.remove_separator_at(ctx.child_idx);
    if (!remove_parent_sep_result) {
        pager->unref_page(static_cast<int>(*ctx.right_sibling_page_num));
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }

    current_leaf.write_back();
    parent_page.write_back();
    record_page_effect(action, PageEffectKind::Write, underflow_page_num, current_leaf.data());
    record_page_effect(action, PageEffectKind::Write, ctx.parent_page_num, parent_page.data());

    PagerMutationResult free_result = pager->free_page(
        operation,
        *ctx.right_sibling_page_num);
    if (free_result.status != PagerResult::Success) {
        pager->unref_page(static_cast<int>(*ctx.right_sibling_page_num));
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }
    record_pager_effects(action, std::move(free_result.effects));

    pager->unref_page(static_cast<int>(*ctx.right_sibling_page_num));
    pager->unref_page(underflow_page_num);
    return finish_parent_after_merge(
        parent_page,
        ctx.parent_page_num,
        path,
        operation,
        action);
}

BTreeStatus BTree::merge_with_left_leaf(
    BLeafPage &current_leaf,
    BInternalPage &parent_page,
    const MergeParentContext &ctx,
    std::uint32_t underflow_page_num,
    std::vector<TraversalPathEntry> &path,
    BTreeOperation &operation,
    PendingBTreeAction *action
) {
    if (ctx.left_sibling_page_num == std::nullopt) return BTreeStatus::FailedToRemove;

    // Merge the current leaf into the left sibling.
    // The left sibling survives, the current page gets freed, and the parent loses
    // the separator that used to name the current leaf.
    PagerGetResult get_left_result = pager->get(static_cast<int>(*ctx.left_sibling_page_num));
    if (get_left_result.status != PagerResult::Success) {
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRead;
    }

    BLeafPage left_sibling(get_left_result.page->data.data());
    while (current_leaf.get_key_count() > 0) {
        std::optional<Key> move_key = current_leaf.key_at(0);
        std::optional<Value> move_value = current_leaf.get_at(0);
        if (move_key == std::nullopt || move_value == std::nullopt) {
            pager->unref_page(static_cast<int>(*ctx.left_sibling_page_num));
            pager->unref_page(ctx.parent_page_num);
            pager->unref_page(underflow_page_num);
            return BTreeStatus::FailedToRemove;
        }

        bool insert_result = left_sibling.insert_at(left_sibling.get_key_count(), *move_key, *move_value);
        bool remove_result = current_leaf.remove_at(0);
        if (!insert_result || !remove_result) {
            pager->unref_page(static_cast<int>(*ctx.left_sibling_page_num));
            pager->unref_page(ctx.parent_page_num);
            pager->unref_page(underflow_page_num);
            return BTreeStatus::FailedToRemove;
        }
    }

    bool remove_parent_sep_result = parent_page.remove_separator_at(ctx.child_idx - 1);
    if (!remove_parent_sep_result) {
        pager->unref_page(static_cast<int>(*ctx.left_sibling_page_num));
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }

    left_sibling.write_back();
    parent_page.write_back();
    record_page_effect(action, PageEffectKind::Write, *ctx.left_sibling_page_num, left_sibling.data());
    record_page_effect(action, PageEffectKind::Write, ctx.parent_page_num, parent_page.data());

    PagerMutationResult free_result = pager->free_page(
        operation,
        underflow_page_num);
    if (free_result.status != PagerResult::Success) {
        pager->unref_page(static_cast<int>(*ctx.left_sibling_page_num));
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }
    record_pager_effects(action, std::move(free_result.effects));

    pager->unref_page(static_cast<int>(*ctx.left_sibling_page_num));
    pager->unref_page(underflow_page_num);
    return finish_parent_after_merge(
        parent_page,
        ctx.parent_page_num,
        path,
        operation,
        action);
}

BTreeStatus BTree::borrow_from_right_internal(
    BInternalPage &current_page,
    BInternalPage &parent_page,
    const MergeParentContext &ctx,
    std::uint32_t underflow_page_num,
    PendingBTreeAction *action
) {
    if (ctx.right_sibling_page_num == std::nullopt) return BTreeStatus::FailedToRemove;

    // Load the right internal sibling and make sure it can lend one separator.
    PagerGetResult get_right_result = pager->get(static_cast<int>(*ctx.right_sibling_page_num));
    if (get_right_result.status != PagerResult::Success) {
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRead;
    }

    BInternalPage right_sibling(get_right_result.page->data.data());
    if (right_sibling.get_key_count() <= MIN_KEYS(BTREE_ORDER)) {
        pager->unref_page(static_cast<int>(*ctx.right_sibling_page_num));
        return BTreeStatus::FailedToRemove;
    }

    PagerResult begin_write_right_result = pager->begin_write(static_cast<int>(*ctx.right_sibling_page_num));
    if (begin_write_right_result != PagerResult::Success) {
        pager->unref_page(static_cast<int>(*ctx.right_sibling_page_num));
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }

    // Rotate through the parent:
    // - parent separator moves down into the current page
    // - the right sibling's old leftmost child comes with it
    // - the right sibling's old first separator moves up to become the new parent separator
    std::optional<Key> parent_sep = parent_page.key_at(ctx.child_idx);
    std::optional<std::uint32_t> right_old_leftmost_child = right_sibling.get_leftmost_child();
    std::optional<Key> new_parent_sep = right_sibling.key_at(0);
    std::optional<std::uint32_t> right_new_leftmost_child = right_sibling.get_right_child(0);
    if (parent_sep == std::nullopt || right_old_leftmost_child == std::nullopt ||
        new_parent_sep == std::nullopt || right_new_leftmost_child == std::nullopt) {
        pager->unref_page(static_cast<int>(*ctx.right_sibling_page_num));
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }

    bool insert_result = current_page.insert_separator_at(
        current_page.get_key_count(),
        *parent_sep,
        *right_old_leftmost_child
    );
    bool replace_leftmost_result = right_sibling.replace_leftmost_child(*right_new_leftmost_child);
    bool remove_sep_result = right_sibling.remove_separator_at(0);
    bool set_parent_sep_result = parent_page.set_separator_key_at(ctx.child_idx, *new_parent_sep);
    if (!insert_result || !replace_leftmost_result || !remove_sep_result || !set_parent_sep_result) {
        pager->unref_page(static_cast<int>(*ctx.right_sibling_page_num));
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }

    current_page.write_back();
    right_sibling.write_back();
    parent_page.write_back();
    record_page_effect(action, PageEffectKind::Write, underflow_page_num, current_page.data());
    record_page_effect(action, PageEffectKind::Write, *ctx.right_sibling_page_num, right_sibling.data());
    record_page_effect(action, PageEffectKind::Write, ctx.parent_page_num, parent_page.data());

    pager->unref_page(static_cast<int>(*ctx.right_sibling_page_num));
    pager->unref_page(ctx.parent_page_num);
    pager->unref_page(underflow_page_num);
    return BTreeStatus::Success;
}

BTreeStatus BTree::borrow_from_left_internal(
    BInternalPage &current_page,
    BInternalPage &parent_page,
    const MergeParentContext &ctx,
    std::uint32_t underflow_page_num,
    PendingBTreeAction *action
) {
    if (ctx.left_sibling_page_num == std::nullopt) return BTreeStatus::FailedToRemove;

    // Load the left internal sibling and make sure it can lend one separator.
    PagerGetResult get_left_result = pager->get(static_cast<int>(*ctx.left_sibling_page_num));
    if (get_left_result.status != PagerResult::Success) {
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRead;
    }

    BInternalPage left_sibling(get_left_result.page->data.data());
    if (left_sibling.get_key_count() <= MIN_KEYS(BTREE_ORDER)) {
        pager->unref_page(static_cast<int>(*ctx.left_sibling_page_num));
        return BTreeStatus::FailedToRemove;
    }

    PagerResult begin_write_left_result = pager->begin_write(static_cast<int>(*ctx.left_sibling_page_num));
    if (begin_write_left_result != PagerResult::Success) {
        pager->unref_page(static_cast<int>(*ctx.left_sibling_page_num));
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }

    // Rotate through the parent from the left side:
    // - parent separator moves down into the current page as its first separator
    // - the left sibling's old rightmost child becomes the new leftmost child of the current page
    // - the left sibling's old rightmost separator moves up to become the new parent separator
    std::size_t left_last_idx = left_sibling.get_key_count() - 1;
    std::optional<Key> parent_sep = parent_page.key_at(ctx.child_idx - 1);
    std::optional<Key> borrowed_sep = left_sibling.key_at(left_last_idx);
    std::optional<std::uint32_t> borrowed_child = left_sibling.get_right_child(left_last_idx);
    std::optional<std::uint32_t> old_current_leftmost = current_page.get_leftmost_child();
    if (parent_sep == std::nullopt || borrowed_sep == std::nullopt ||
        borrowed_child == std::nullopt || old_current_leftmost == std::nullopt) {
        pager->unref_page(static_cast<int>(*ctx.left_sibling_page_num));
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }

    bool replace_leftmost_result = current_page.replace_leftmost_child(*borrowed_child);
    bool insert_result = current_page.insert_separator_at(0, *parent_sep, *old_current_leftmost);
    bool set_parent_sep_result = parent_page.set_separator_key_at(ctx.child_idx - 1, *borrowed_sep);
    bool remove_sep_result = left_sibling.remove_separator_at(left_last_idx);
    if (!replace_leftmost_result || !insert_result || !set_parent_sep_result || !remove_sep_result) {
        pager->unref_page(static_cast<int>(*ctx.left_sibling_page_num));
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }

    left_sibling.write_back();
    current_page.write_back();
    parent_page.write_back();
    record_page_effect(action, PageEffectKind::Write, *ctx.left_sibling_page_num, left_sibling.data());
    record_page_effect(action, PageEffectKind::Write, underflow_page_num, current_page.data());
    record_page_effect(action, PageEffectKind::Write, ctx.parent_page_num, parent_page.data());

    pager->unref_page(static_cast<int>(*ctx.left_sibling_page_num));
    pager->unref_page(ctx.parent_page_num);
    pager->unref_page(underflow_page_num);
    return BTreeStatus::Success;
}

BTreeStatus BTree::merge_with_right_internal(
    BInternalPage &current_page,
    BInternalPage &parent_page,
    const MergeParentContext &ctx,
    std::uint32_t underflow_page_num,
    std::vector<TraversalPathEntry> &path,
    BTreeOperation &operation,
    PendingBTreeAction *action
) {
    if (ctx.right_sibling_page_num == std::nullopt) return BTreeStatus::FailedToRemove;

    // Merge the right internal page into the current page.
    // The parent separator between them has to move down first, then the rest of the
    // right sibling comes after it.
    PagerGetResult get_right_result = pager->get(static_cast<int>(*ctx.right_sibling_page_num));
    if (get_right_result.status != PagerResult::Success) {
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRead;
    }

    BInternalPage right_sibling(get_right_result.page->data.data());
    std::optional<Key> parent_sep = parent_page.key_at(ctx.child_idx);
    std::optional<std::uint32_t> right_leftmost_child = right_sibling.get_leftmost_child();
    if (parent_sep == std::nullopt || right_leftmost_child == std::nullopt) {
        pager->unref_page(static_cast<int>(*ctx.right_sibling_page_num));
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }

    // Move the parent separator down first, then pull over the rest of the right page.
    bool insert_parent_sep_result = current_page.insert_separator_at(
        current_page.get_key_count(),
        *parent_sep,
        *right_leftmost_child
    );
    if (!insert_parent_sep_result) {
        pager->unref_page(static_cast<int>(*ctx.right_sibling_page_num));
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }

    while (right_sibling.get_key_count() > 0) {
        std::optional<Key> move_key = right_sibling.key_at(0);
        std::optional<std::uint32_t> move_right_child = right_sibling.get_right_child(0);
        if (move_key == std::nullopt || move_right_child == std::nullopt) {
            pager->unref_page(static_cast<int>(*ctx.right_sibling_page_num));
            pager->unref_page(ctx.parent_page_num);
            pager->unref_page(underflow_page_num);
            return BTreeStatus::FailedToRemove;
        }

        bool insert_result = current_page.insert_separator_at(
            current_page.get_key_count(),
            *move_key,
            *move_right_child
        );
        bool new_leftmost_result = true;
        if (right_sibling.get_key_count() > 1) {
            std::optional<std::uint32_t> new_leftmost = right_sibling.get_right_child(0);
            if (new_leftmost == std::nullopt) {
                pager->unref_page(static_cast<int>(*ctx.right_sibling_page_num));
                pager->unref_page(ctx.parent_page_num);
                pager->unref_page(underflow_page_num);
                return BTreeStatus::FailedToRemove;
            }
            new_leftmost_result = right_sibling.replace_leftmost_child(*new_leftmost);
        }
        bool remove_result = right_sibling.remove_separator_at(0);
        if (!insert_result || !new_leftmost_result || !remove_result) {
            pager->unref_page(static_cast<int>(*ctx.right_sibling_page_num));
            pager->unref_page(ctx.parent_page_num);
            pager->unref_page(underflow_page_num);
            return BTreeStatus::FailedToRemove;
        }
    }

    bool remove_parent_sep_result = parent_page.remove_separator_at(ctx.child_idx);
    if (!remove_parent_sep_result) {
        pager->unref_page(static_cast<int>(*ctx.right_sibling_page_num));
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }

    current_page.write_back();
    parent_page.write_back();
    record_page_effect(action, PageEffectKind::Write, underflow_page_num, current_page.data());
    record_page_effect(action, PageEffectKind::Write, ctx.parent_page_num, parent_page.data());

    PagerMutationResult free_result = pager->free_page(
        operation,
        *ctx.right_sibling_page_num);
    if (free_result.status != PagerResult::Success) {
        pager->unref_page(static_cast<int>(*ctx.right_sibling_page_num));
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }
    record_pager_effects(action, std::move(free_result.effects));

    pager->unref_page(static_cast<int>(*ctx.right_sibling_page_num));
    pager->unref_page(underflow_page_num);
    return finish_parent_after_merge(
        parent_page,
        ctx.parent_page_num,
        path,
        operation,
        action);
}

BTreeStatus BTree::merge_with_left_internal(
    BInternalPage &current_page,
    BInternalPage &parent_page,
    const MergeParentContext &ctx,
    std::uint32_t underflow_page_num,
    std::vector<TraversalPathEntry> &path,
    BTreeOperation &operation,
    PendingBTreeAction *action
) {
    if (ctx.left_sibling_page_num == std::nullopt) return BTreeStatus::FailedToRemove;

    // Merge the current internal page into the left sibling.
    // The parent separator between them moves down into the left sibling first, then
    // the rest of the current page follows after it.
    PagerGetResult get_left_result = pager->get(static_cast<int>(*ctx.left_sibling_page_num));
    if (get_left_result.status != PagerResult::Success) {
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRead;
    }

    BInternalPage left_sibling(get_left_result.page->data.data());
    std::optional<Key> parent_sep = parent_page.key_at(ctx.child_idx - 1);
    std::optional<std::uint32_t> current_leftmost_child = current_page.get_leftmost_child();
    if (parent_sep == std::nullopt || current_leftmost_child == std::nullopt) {
        pager->unref_page(static_cast<int>(*ctx.left_sibling_page_num));
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }

    bool insert_parent_sep_result = left_sibling.insert_separator_at(
        left_sibling.get_key_count(),
        *parent_sep,
        *current_leftmost_child
    );
    if (!insert_parent_sep_result) {
        pager->unref_page(static_cast<int>(*ctx.left_sibling_page_num));
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }

    while (current_page.get_key_count() > 0) {
        std::optional<Key> move_key = current_page.key_at(0);
        std::optional<std::uint32_t> move_right_child = current_page.get_right_child(0);
        if (move_key == std::nullopt || move_right_child == std::nullopt) {
            pager->unref_page(static_cast<int>(*ctx.left_sibling_page_num));
            pager->unref_page(ctx.parent_page_num);
            pager->unref_page(underflow_page_num);
            return BTreeStatus::FailedToRemove;
        }

        bool insert_result = left_sibling.insert_separator_at(
            left_sibling.get_key_count(),
            *move_key,
            *move_right_child
        );
        bool new_leftmost_result = true;
        if (current_page.get_key_count() > 1) {
            std::optional<std::uint32_t> new_leftmost = current_page.get_right_child(0);
            if (new_leftmost == std::nullopt) {
                pager->unref_page(static_cast<int>(*ctx.left_sibling_page_num));
                pager->unref_page(ctx.parent_page_num);
                pager->unref_page(underflow_page_num);
                return BTreeStatus::FailedToRemove;
            }
            new_leftmost_result = current_page.replace_leftmost_child(*new_leftmost);
        }
        bool remove_result = current_page.remove_separator_at(0);
        if (!insert_result || !new_leftmost_result || !remove_result) {
            pager->unref_page(static_cast<int>(*ctx.left_sibling_page_num));
            pager->unref_page(ctx.parent_page_num);
            pager->unref_page(underflow_page_num);
            return BTreeStatus::FailedToRemove;
        }
    }

    bool remove_parent_sep_result = parent_page.remove_separator_at(ctx.child_idx - 1);
    if (!remove_parent_sep_result) {
        pager->unref_page(static_cast<int>(*ctx.left_sibling_page_num));
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }

    left_sibling.write_back();
    parent_page.write_back();
    record_page_effect(action, PageEffectKind::Write, *ctx.left_sibling_page_num, left_sibling.data());
    record_page_effect(action, PageEffectKind::Write, ctx.parent_page_num, parent_page.data());

    PagerMutationResult free_result = pager->free_page(
        operation,
        underflow_page_num);
    if (free_result.status != PagerResult::Success) {
        pager->unref_page(static_cast<int>(*ctx.left_sibling_page_num));
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }
    record_pager_effects(action, std::move(free_result.effects));

    pager->unref_page(static_cast<int>(*ctx.left_sibling_page_num));
    pager->unref_page(underflow_page_num);
    return finish_parent_after_merge(
        parent_page,
        ctx.parent_page_num,
        path,
        operation,
        action);
}

BTreeStatus BTree::propagate_merging(
    std::uint32_t underflow_page_num,
    std::vector<TraversalPathEntry> &path,
    BTreeOperation &operation,
    PendingBTreeAction *action
) {
    /**
     * Repair an underflow after delete.
     *
     * Order of attack:
     * - handle the root special case
     * - try borrowing from the right sibling
     * - then try borrowing from the left sibling
     * - if neither can lend, merge with the right sibling if it exists
     * - otherwise merge with the left sibling
     */
    // Load the underflowing page first so we can inspect whether we are dealing
    // with a leaf or an internal page.
    operation.lock_exclusive(underflow_page_num);
    PagerGetResult get_underflow_result = pager->get(underflow_page_num);
    if (get_underflow_result.status != PagerResult::Success) return BTreeStatus::FailedToRead;

    PageType underflow_page_type = BTreePage::peek_page_type(get_underflow_result.page->data.data());

    // Roots are handled separately because they are allowed to break the normal
    // min-key invariant.
    if (path.empty()) {
        return handle_root_underflow(
            underflow_page_num,
            underflow_page_type,
            get_underflow_result.page->data.data(),
            operation,
            action);
    }

    // Load the parent and compute the sibling context around the underflowing page.
    TraversalPathEntry parent_path_entry = path.back();
    operation.lock_exclusive(parent_path_entry.parent_page_num);
    PagerGetResult get_parent_result = pager->get(parent_path_entry.parent_page_num);
    if (get_parent_result.status != PagerResult::Success) {
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRead;
    }

    PagerResult begin_write_parent_result = pager->begin_write(parent_path_entry.parent_page_num);
    if (begin_write_parent_result != PagerResult::Success) {
        pager->unref_page(parent_path_entry.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }

    BInternalPage parent_page(get_parent_result.page->data.data());
    MergeParentContext ctx = build_merge_parent_context(parent_path_entry, parent_page);

    // Siblings are acquired in page-number order so two structural repairs
    // cannot take the same sibling group in opposite orders.
    std::vector<std::uint32_t> sibling_page_nums;
    if (ctx.left_sibling_page_num) sibling_page_nums.push_back(*ctx.left_sibling_page_num);
    if (ctx.right_sibling_page_num) sibling_page_nums.push_back(*ctx.right_sibling_page_num);
    std::sort(sibling_page_nums.begin(), sibling_page_nums.end());
    for (std::uint32_t sibling_page_num : sibling_page_nums) {
        operation.lock_exclusive(sibling_page_num);
    }

    if (underflow_page_type == PageType::Leaf) {
        BLeafPage current_leaf(get_underflow_result.page->data.data());

        // Try to repair the leaf by borrowing before we merge anything.
        if (ctx.right_sibling_page_num != std::nullopt) {
            BTreeStatus right_borrow_result = borrow_from_right_leaf(
                current_leaf,
                parent_page,
                ctx,
                underflow_page_num,
                action);
            if (right_borrow_result == BTreeStatus::Success) return BTreeStatus::Success;
            if (right_borrow_result == BTreeStatus::FailedToRead) return right_borrow_result;
        }

        if (ctx.left_sibling_page_num != std::nullopt) {
            BTreeStatus left_borrow_result = borrow_from_left_leaf(
                current_leaf,
                parent_page,
                ctx,
                underflow_page_num,
                action);
            if (left_borrow_result == BTreeStatus::Success) return BTreeStatus::Success;
            if (left_borrow_result == BTreeStatus::FailedToRead) return left_borrow_result;
        }

        // If borrowing failed from both sides, we have to merge.
        if (ctx.right_sibling_page_num != std::nullopt) {
            return merge_with_right_leaf(
                current_leaf,
                parent_page,
                ctx,
                underflow_page_num,
                path,
                operation,
                action);
        }
        return merge_with_left_leaf(
            current_leaf,
            parent_page,
            ctx,
            underflow_page_num,
            path,
            operation,
            action);
    }

    BInternalPage current_page(get_underflow_result.page->data.data());

    // Same flow for internal pages: try borrowing first, then fall back to merging.
    if (ctx.right_sibling_page_num != std::nullopt) {
        BTreeStatus right_borrow_result = borrow_from_right_internal(
            current_page,
            parent_page,
            ctx,
            underflow_page_num,
            action);
        if (right_borrow_result == BTreeStatus::Success) return BTreeStatus::Success;
        if (right_borrow_result == BTreeStatus::FailedToRead) return right_borrow_result;
    }

    if (ctx.left_sibling_page_num != std::nullopt) {
        BTreeStatus left_borrow_result = borrow_from_left_internal(
            current_page,
            parent_page,
            ctx,
            underflow_page_num,
            action);
        if (left_borrow_result == BTreeStatus::Success) return BTreeStatus::Success;
        if (left_borrow_result == BTreeStatus::FailedToRead) return left_borrow_result;
    }

    if (ctx.right_sibling_page_num != std::nullopt) {
        return merge_with_right_internal(
            current_page,
            parent_page,
            ctx,
            underflow_page_num,
            path,
            operation,
            action);
    }
    return merge_with_left_internal(
        current_page,
        parent_page,
        ctx,
        underflow_page_num,
        path,
        operation,
        action);
}

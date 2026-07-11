#include <BTree.h>
#include <Pager.h>
#include <cassert>
BTree::~BTree() {
    close();
}

BTreeStatus BTree::open(std::string db_file) {
    assert(!pager_open && pager == nullptr);

    pager = new Pager();
    PagerResult open_result = pager->open(db_file);
    if (open_result != PagerResult::Success) {
        delete pager;
        pager = nullptr;
        return BTreeStatus::FailedToOpenDB;
    }

    currently_open_db_file = std::move(db_file);
    pager_open = true;
    return BTreeStatus::Success;
}

BTreeStatus BTree::close() {
    // If we are already closed, there is nothing to do.
    if (!pager_open && pager == nullptr) return BTreeStatus::Success;

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

    // Let the pager unwind either the in-memory write set or the durable journal.
    PagerResult rollback_result = pager->rollback_transaction();
    if (rollback_result != PagerResult::Success) return BTreeRollbackStatus::Failed;

    return BTreeRollbackStatus::Success;
}


BTreeGetStatus BTree::get(std::uint64_t key) {
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
    get_result.status = BTreeStatus::Success;

    // Descend from the root tohe leaf. We pass false to the second arg
    // Since we are not interested in keeping the parent pointers
    LeafDescentResult descent_result = descend_from_root_to_leaf(key, false);
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
    std::optional<std::uint64_t> key_at_idx = target_leaf.key_at(idx);
    if (key_at_idx == std::nullopt || key_at_idx != key) {
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

BTreeStatus BTree::insert(std::uint64_t key, Value &value) {
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
    // Descend from the root to the target leaf.
    // We need the path this time in case the insert changes separators or causes a split.
    LeafDescentResult descent_result = descend_from_root_to_leaf(key, true);
    if (descent_result.status == BTreeStatus::EmptyTree) {
        PagerAllocateResult allocation_result = pager->allocate_page();
        if (allocation_result.status != PagerResult::Success) return BTreeStatus::FailedToInsert;

        std::uint32_t root_page_num = allocation_result.page_num;
        BLeafPage::fill_initial_layout(allocation_result.data);
        BLeafPage root_leaf(allocation_result.data);

        bool insert_result = root_leaf.insert_at(0, key, value);
        if (!insert_result) {
            pager->unref_page(root_page_num);
            return BTreeStatus::FailedToInsert;
        }

        root_leaf.write_back();
        PagerResult set_root_result = pager->set_btree_root(root_page_num);
        pager->unref_page(root_page_num);
        if (set_root_result != PagerResult::Success) return BTreeStatus::FailedToInsert;
        return BTreeStatus::Success;
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
    std::optional<std::uint64_t> key_at_idx = target_leaf.key_at(idx);

    // If the key already exists, this insert is really just an overwrite of the stored value.
    if (key_at_idx != std::nullopt && *key_at_idx == key) {
        bool set_result = target_leaf.set(key, value);
        if (!set_result) {
            pager->unref_page(descent_result.leaf_page_num);
            return BTreeStatus::FailedToInsert;
        }

        target_leaf.write_back();
        pager->unref_page(descent_result.leaf_page_num);
        return BTreeStatus::Success;
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

    if (needs_split) {
        BTreeStatus split_result = propagate_splitting(
            descent_result.leaf_page_num,
            descent_result.path
        );
        pager->unref_page(descent_result.leaf_page_num);
        return split_result;
    }

    pager->unref_page(descent_result.leaf_page_num);
    return BTreeStatus::Success;
}

BTreeRemoveStatus BTree::remove(std::uint64_t key) {
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
    LeafDescentResult descent_result = descend_from_root_to_leaf(key, true);
    if (descent_result.status == BTreeStatus::EmptyTree) {
        remove_result.status = BTreeStatus::KeyNotInTree;
        return remove_result;
    }

    if (descent_result.status != BTreeStatus::Success) {
        remove_result.status = descent_result.status;
        return remove_result;
    }

    // Parse the leaf page and find the key
    BLeafPage target_leaf_page(descent_result.leaf_page);
    std::uint32_t leaf_page_num = descent_result.leaf_page_num;
    std::size_t key_idx = target_leaf_page.lower_bound_key(key);
    if (key_idx == target_leaf_page.get_key_count() || *target_leaf_page.key_at(key_idx) != key) {
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
    
    // Now check if we went under the min size
    if (target_leaf_page.get_key_count() < MIN_KEYS(BTREE_ORDER)) {
        BTreeStatus merging_status = propagate_merging(leaf_page_num, descent_result.path);
        pager->unref_page(leaf_page_num);
        remove_result.status = merging_status;
        return remove_result;
    }

    // We are assuming in practice that deleting a key from a non-root leaf node will leave at least one key left in the node
    // That's so we don't have to deal with empty leaf nodes and removing the corresponding separator key

    // If the first key got deleted, propagate it up
    if (is_first_key) {
        BTreeStatus separator_change_status = propagate_separator_change_upward(descent_result.path, *target_leaf_page.key_at(0));
        pager->unref_page(leaf_page_num);
        remove_result.status = separator_change_status;
        return remove_result;
    }

    pager->unref_page(leaf_page_num);

    return remove_result;
}



LeafDescentResult BTree::descend_from_root_to_leaf(std::uint64_t key, bool include_path) {
    LeafDescentResult descent_result{};
    PagerGetRootResult root_get_result = pager->get_btree_root();

    if (root_get_result.status == PagerResult::EmptyBTree) {
        descent_result.status = BTreeStatus::EmptyTree;
        return descent_result;
    }
    if (root_get_result.status != PagerResult::Success) {
        descent_result.status = BTreeStatus::FailedToGetRoot;
        return descent_result;
    }

    // If the root is already a leaf, return the root
    PageType root_page_type = BTreePage::peek_page_type(root_get_result.data);
    if (root_page_type == PageType::Leaf) {
        descent_result.leaf_page = root_get_result.data;
        descent_result.leaf_page_num = root_get_result.root_page_num;
        return descent_result;
    }

    // Otherwise, keep on descending downwards until we hit a leaf
    std::uint32_t curr_page_num = root_get_result.root_page_num;
    char *curr_data = root_get_result.data; 
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
        } else if (*curr.key_at(idx) == key) { 
            // The key at the idx is equal to the target
            target_child_page_num = curr.get_right_child(idx);
            child_dir = ChildDirection::Right;
        } else {
            // The key at the idx is greater than the target
            target_child_page_num = curr.get_left_child(idx);
            child_dir = ChildDirection::Left;
        }

        // Get the computed child page number. That's the next page in our traversal
        PagerGetResult pager_get_result = pager->get(*target_child_page_num);
        pager->unref_page(curr_page_num); // Make sure to unref the parent page since we don't need it anymore.

        // Handle failure of reading
        if (pager_get_result.status != PagerResult::Success) {
            descent_result.status = BTreeStatus::FailedToRead;
            return descent_result;
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
        curr_data = pager_get_result.data;

        // If the next page is a leaf, terminate the search here
        PageType child_page_type = BTreePage::peek_page_type(pager_get_result.data);
        if (child_page_type == PageType::Leaf) break;
    }

    // Finalize result
    descent_result.leaf_page = curr_data;
    descent_result.leaf_page_num = curr_page_num;
    return descent_result;
}

BTreeStatus BTree::propagate_splitting(std::uint32_t split_page_num, std::vector<TraversalPathEntry> &path) {
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

    PageType page_type = BTreePage::peek_page_type(get_split_page_result.data);
    pager->begin_write(split_page_num);
    if (page_type == PageType::Leaf) {
        BLeafPage split_page(get_split_page_result.data);

        PagerAllocateResult allocation_result = pager->allocate_page();
        if (allocation_result.status != PagerResult::Success) {
            pager->unref_page(split_page_num);
            return BTreeStatus::FailedToAllocateNewPage;
        }
        
        std::uint32_t new_leaf_page_num = allocation_result.page_num;
        BLeafPage::fill_initial_layout(allocation_result.data);
        BLeafPage new_leaf_page(allocation_result.data);

        std::size_t median_separator_idx = split_page.get_key_count() / 2;
        BTree::migrate_leaf(split_page, new_leaf_page, median_separator_idx); // Assumes it does the write_back on both

        if (path.size() == 0) {
            // The current node we are splitting is a root node
            // Handle the splitting of the root and return
            BTreeStatus split_root = handle_splitting_root(split_page_num, new_leaf_page_num, *new_leaf_page.key_at(0));
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
        BInternalPage parent_page(get_parent_result.data);
        std::uint64_t key = *new_leaf_page.key_at(0);
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

        pager->unref_page(split_page_num);
        pager->unref_page(new_leaf_page_num);

        if (parent_needs_split) {
            pager->unref_page(parent_path_entry.parent_page_num);
            return propagate_splitting(parent_path_entry.parent_page_num, path);
        }

        pager->unref_page(parent_path_entry.parent_page_num);
        return BTreeStatus::Success;
    } else {
        BInternalPage split_page(get_split_page_result.data);

        // Allocate the new internal page that is going to hold everything to the right
        // of the promoted median separator.
        PagerAllocateResult allocation_result = pager->allocate_page();
        if (allocation_result.status != PagerResult::Success) {
            pager->unref_page(split_page_num);
            return BTreeStatus::FailedToAllocateNewPage;
        }

        std::uint32_t new_internal_page_num = allocation_result.page_num;
        BInternalPage::fill_initial_layout(allocation_result.data);
        BInternalPage new_internal_page(allocation_result.data);

        // Capture the median key before mutating the original page.
        // This is the key that gets promoted upward and does not stay in either child page.
        std::size_t median_separator_idx = split_page.get_key_count() / 2;
        std::optional<std::uint64_t> promoted_key = split_page.key_at(median_separator_idx);
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

        if (path.size() == 0) {
            // The current internal page being split is the root.
            // Create a new root above the two split children.
            BTreeStatus split_root = handle_splitting_root(split_page_num, new_internal_page_num, *promoted_key);
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

        BInternalPage parent_page(get_parent_result.data);
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

        pager->unref_page(split_page_num);
        pager->unref_page(new_internal_page_num);

        if (parent_needs_split) {
            pager->unref_page(parent_path_entry.parent_page_num);
            return propagate_splitting(parent_path_entry.parent_page_num, path);
        }

        pager->unref_page(parent_path_entry.parent_page_num);
        return BTreeStatus::Success;
    }

    return BTreeStatus::FailedToInsert;
}

void BTree::migrate_leaf(BLeafPage src, BLeafPage dst, std::size_t separator_idx) {
    // Move every key-value pair starting at the split point into the new right leaf.
    // We keep removing from the same logical index because the left leaf shrinks after each move.
    while (src.get_key_count() > separator_idx) {
        std::optional<std::uint64_t> move_key = src.key_at(separator_idx);
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

void BTree::migrate_internal(BInternalPage src, BInternalPage dst, std::size_t median_separator_idx) {
    // The child immediately to the right of the promoted median becomes the leftmost
    // child of the new internal page.
    std::optional<std::uint32_t> new_leftmost_child = src.get_right_child(median_separator_idx);
    if (new_leftmost_child == std::nullopt) std::abort();

    bool set_leftmost_result = dst.set_leftmost_child(*new_leftmost_child);
    if (!set_leftmost_result) std::abort();

    // Move every separator strictly to the right of the median into the new right page.
    // We keep removing from the same logical index because the vector shrinks after each removal.
    while (src.get_key_count() > median_separator_idx + 1) {
        std::optional<std::uint64_t> move_key = src.key_at(median_separator_idx + 1);
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
    std::uint64_t separator_key
) {
    // A root split means we need to allocate a brand new internal root above the
    // two children that just came out of the split.
    PagerAllocateResult allocation_result = pager->allocate_page();
    if (allocation_result.status != PagerResult::Success) return BTreeStatus::FailedToAllocateNewPage;

    std::uint32_t new_root_page_num = allocation_result.page_num;
    BInternalPage::fill_initial_layout(allocation_result.data);
    BInternalPage new_root_page(allocation_result.data);

    // The old root becomes the leftmost child, and the split-off page becomes the child
    // to the right of the one separator key stored in the new root.
    bool set_leftmost_result = new_root_page.set_leftmost_child(left_child_page_num);
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

    PagerResult set_root_result = pager->set_btree_root(new_root_page_num);
    pager->unref_page(new_root_page_num);
    if (set_root_result != PagerResult::Success) return BTreeStatus::FailedToInsert;

    return BTreeStatus::Success;
}

BTreeStatus BTree::propagate_separator_change_upward(
    const std::vector<TraversalPathEntry> &path,
    std::uint64_t new_subtree_min
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
            PagerGetResult get_parent_result = pager->get(entry.parent_page_num);
            if (get_parent_result.status != PagerResult::Success) return BTreeStatus::FailedToRead;

            PagerResult begin_write_result = pager->begin_write(entry.parent_page_num);
            if (begin_write_result != PagerResult::Success) {
                pager->unref_page(entry.parent_page_num);
                return BTreeStatus::FailedToRemove;
            }

            BInternalPage parent_page(get_parent_result.data);
            bool set_result = parent_page.set_separator_key_at(entry.separator_index_used, new_subtree_min);
            if (!set_result) {
                pager->unref_page(entry.parent_page_num);
                return BTreeStatus::FailedToRemove;
            }

            parent_page.write_back();
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
        PagerGetResult get_parent_result = pager->get(entry.parent_page_num);
        if (get_parent_result.status != PagerResult::Success) return BTreeStatus::FailedToRead;

        PagerResult begin_write_result = pager->begin_write(entry.parent_page_num);
        if (begin_write_result != PagerResult::Success) {
            pager->unref_page(entry.parent_page_num);
            return BTreeStatus::FailedToRemove;
        }

        BInternalPage parent_page(get_parent_result.data);
        bool set_result = parent_page.set_separator_key_at(entry.separator_index_used - 1, new_subtree_min);
        if (!set_result) {
            pager->unref_page(entry.parent_page_num);
            return BTreeStatus::FailedToRemove;
        }

        parent_page.write_back();
        pager->unref_page(entry.parent_page_num);
        return BTreeStatus::Success;
    }

    return BTreeStatus::Success;
}
BTreeStatus BTree::handle_root_underflow(std::uint32_t underflow_page_num, PageType underflow_page_type, char *underflow_page_data) {
    // The root is special. It is allowed to violate the usual min-size rules.
    if (underflow_page_type == PageType::Leaf) {
        BLeafPage root_leaf(underflow_page_data);

        // If this leaf root still has data in it, nothing to repair.
        if (root_leaf.get_key_count() > 0) {
            pager->unref_page(underflow_page_num);
            return BTreeStatus::Success;
        }

        // The tree became empty. Clear the root pointer in the header and free this page.
        PagerResult clear_root_result = pager->set_btree_root(0);
        if (clear_root_result != PagerResult::Success) {
            pager->unref_page(underflow_page_num);
            return BTreeStatus::FailedToRemove;
        }

        PagerResult free_root_result = pager->free_page(underflow_page_num);
        pager->unref_page(underflow_page_num);
        if (free_root_result != PagerResult::Success) return BTreeStatus::FailedToRemove;
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

    PagerResult set_root_result = pager->set_btree_root(*only_child_page_num);
    if (set_root_result != PagerResult::Success) {
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }

    PagerResult free_root_result = pager->free_page(underflow_page_num);
    pager->unref_page(underflow_page_num);
    if (free_root_result != PagerResult::Success) return BTreeStatus::FailedToRemove;
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

BTreeStatus BTree::finish_parent_after_merge(BInternalPage &parent_page, std::uint32_t parent_page_num, std::vector<TraversalPathEntry> &path) {
    // Merging deleted one separator from the parent. If that pushed the parent below
    // the minimum, keep repairing upward recursively.
    bool parent_underflow = parent_page.get_key_count() < MIN_KEYS(BTREE_ORDER);
    path.pop_back();
    pager->unref_page(parent_page_num);
    if (parent_underflow) return propagate_merging(parent_page_num, path);
    return BTreeStatus::Success;
}

BTreeStatus BTree::borrow_from_right_leaf(BLeafPage &current_leaf, BInternalPage &parent_page, const MergeParentContext &ctx, std::uint32_t underflow_page_num) {
    if (ctx.right_sibling_page_num == std::nullopt) return BTreeStatus::FailedToRemove;

    // Load the right sibling and first make sure it can actually spare a key.
    PagerGetResult get_right_result = pager->get(static_cast<int>(*ctx.right_sibling_page_num));
    if (get_right_result.status != PagerResult::Success) {
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRead;
    }

    BLeafPage right_sibling(get_right_result.data);
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
    std::optional<std::uint64_t> borrowed_key = right_sibling.key_at(0);
    std::optional<Value> borrowed_value = right_sibling.get_at(0);
    if (borrowed_key == std::nullopt || borrowed_value == std::nullopt) {
        pager->unref_page(static_cast<int>(*ctx.right_sibling_page_num));
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }

    bool insert_result = current_leaf.insert_at(current_leaf.get_key_count(), *borrowed_key, *borrowed_value);
    bool remove_result = right_sibling.remove_at(0);
    std::optional<std::uint64_t> new_right_first_key = right_sibling.first_key();
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

    pager->unref_page(static_cast<int>(*ctx.right_sibling_page_num));
    pager->unref_page(ctx.parent_page_num);
    pager->unref_page(underflow_page_num);
    return BTreeStatus::Success;
}

BTreeStatus BTree::borrow_from_left_leaf(BLeafPage &current_leaf, BInternalPage &parent_page, const MergeParentContext &ctx, std::uint32_t underflow_page_num) {
    if (ctx.left_sibling_page_num == std::nullopt) return BTreeStatus::FailedToRemove;

    // Load the left sibling and make sure it can actually lend.
    PagerGetResult get_left_result = pager->get(static_cast<int>(*ctx.left_sibling_page_num));
    if (get_left_result.status != PagerResult::Success) {
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRead;
    }

    BLeafPage left_sibling(get_left_result.data);
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
    std::optional<std::uint64_t> borrowed_key = left_sibling.key_at(left_last_idx);
    std::optional<Value> borrowed_value = left_sibling.get_at(left_last_idx);
    if (borrowed_key == std::nullopt || borrowed_value == std::nullopt) {
        pager->unref_page(static_cast<int>(*ctx.left_sibling_page_num));
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }

    bool remove_result = left_sibling.remove_at(left_last_idx);
    bool insert_result = current_leaf.insert_at(0, *borrowed_key, *borrowed_value);
    std::optional<std::uint64_t> new_current_first_key = current_leaf.first_key();
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

    pager->unref_page(static_cast<int>(*ctx.left_sibling_page_num));
    pager->unref_page(ctx.parent_page_num);
    pager->unref_page(underflow_page_num);
    return BTreeStatus::Success;
}

BTreeStatus BTree::merge_with_right_leaf(BLeafPage &current_leaf, BInternalPage &parent_page, const MergeParentContext &ctx, std::uint32_t underflow_page_num, std::vector<TraversalPathEntry> &path) {
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

    BLeafPage right_sibling(get_right_result.data);
    while (right_sibling.get_key_count() > 0) {
        std::optional<std::uint64_t> move_key = right_sibling.key_at(0);
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

    PagerResult free_result = pager->free_page(static_cast<int>(*ctx.right_sibling_page_num));
    if (free_result != PagerResult::Success) {
        pager->unref_page(static_cast<int>(*ctx.right_sibling_page_num));
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }

    pager->unref_page(static_cast<int>(*ctx.right_sibling_page_num));
    pager->unref_page(underflow_page_num);
    return finish_parent_after_merge(parent_page, ctx.parent_page_num, path);
}

BTreeStatus BTree::merge_with_left_leaf(BLeafPage &current_leaf, BInternalPage &parent_page, const MergeParentContext &ctx, std::uint32_t underflow_page_num, std::vector<TraversalPathEntry> &path) {
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

    BLeafPage left_sibling(get_left_result.data);
    while (current_leaf.get_key_count() > 0) {
        std::optional<std::uint64_t> move_key = current_leaf.key_at(0);
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

    PagerResult free_result = pager->free_page(underflow_page_num);
    if (free_result != PagerResult::Success) {
        pager->unref_page(static_cast<int>(*ctx.left_sibling_page_num));
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }

    pager->unref_page(static_cast<int>(*ctx.left_sibling_page_num));
    pager->unref_page(underflow_page_num);
    return finish_parent_after_merge(parent_page, ctx.parent_page_num, path);
}

BTreeStatus BTree::borrow_from_right_internal(BInternalPage &current_page, BInternalPage &parent_page, const MergeParentContext &ctx, std::uint32_t underflow_page_num) {
    if (ctx.right_sibling_page_num == std::nullopt) return BTreeStatus::FailedToRemove;

    // Load the right internal sibling and make sure it can lend one separator.
    PagerGetResult get_right_result = pager->get(static_cast<int>(*ctx.right_sibling_page_num));
    if (get_right_result.status != PagerResult::Success) {
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRead;
    }

    BInternalPage right_sibling(get_right_result.data);
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
    std::optional<std::uint64_t> parent_sep = parent_page.key_at(ctx.child_idx);
    std::optional<std::uint32_t> right_old_leftmost_child = right_sibling.get_leftmost_child();
    std::optional<std::uint64_t> new_parent_sep = right_sibling.key_at(0);
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

    pager->unref_page(static_cast<int>(*ctx.right_sibling_page_num));
    pager->unref_page(ctx.parent_page_num);
    pager->unref_page(underflow_page_num);
    return BTreeStatus::Success;
}

BTreeStatus BTree::borrow_from_left_internal(BInternalPage &current_page, BInternalPage &parent_page, const MergeParentContext &ctx, std::uint32_t underflow_page_num) {
    if (ctx.left_sibling_page_num == std::nullopt) return BTreeStatus::FailedToRemove;

    // Load the left internal sibling and make sure it can lend one separator.
    PagerGetResult get_left_result = pager->get(static_cast<int>(*ctx.left_sibling_page_num));
    if (get_left_result.status != PagerResult::Success) {
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRead;
    }

    BInternalPage left_sibling(get_left_result.data);
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
    std::optional<std::uint64_t> parent_sep = parent_page.key_at(ctx.child_idx - 1);
    std::optional<std::uint64_t> borrowed_sep = left_sibling.key_at(left_last_idx);
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

    pager->unref_page(static_cast<int>(*ctx.left_sibling_page_num));
    pager->unref_page(ctx.parent_page_num);
    pager->unref_page(underflow_page_num);
    return BTreeStatus::Success;
}

BTreeStatus BTree::merge_with_right_internal(BInternalPage &current_page, BInternalPage &parent_page, const MergeParentContext &ctx, std::uint32_t underflow_page_num, std::vector<TraversalPathEntry> &path) {
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

    BInternalPage right_sibling(get_right_result.data);
    std::optional<std::uint64_t> parent_sep = parent_page.key_at(ctx.child_idx);
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
        std::optional<std::uint64_t> move_key = right_sibling.key_at(0);
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

    PagerResult free_result = pager->free_page(static_cast<int>(*ctx.right_sibling_page_num));
    if (free_result != PagerResult::Success) {
        pager->unref_page(static_cast<int>(*ctx.right_sibling_page_num));
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }

    pager->unref_page(static_cast<int>(*ctx.right_sibling_page_num));
    pager->unref_page(underflow_page_num);
    return finish_parent_after_merge(parent_page, ctx.parent_page_num, path);
}

BTreeStatus BTree::merge_with_left_internal(BInternalPage &current_page, BInternalPage &parent_page, const MergeParentContext &ctx, std::uint32_t underflow_page_num, std::vector<TraversalPathEntry> &path) {
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

    BInternalPage left_sibling(get_left_result.data);
    std::optional<std::uint64_t> parent_sep = parent_page.key_at(ctx.child_idx - 1);
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
        std::optional<std::uint64_t> move_key = current_page.key_at(0);
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

    PagerResult free_result = pager->free_page(underflow_page_num);
    if (free_result != PagerResult::Success) {
        pager->unref_page(static_cast<int>(*ctx.left_sibling_page_num));
        pager->unref_page(ctx.parent_page_num);
        pager->unref_page(underflow_page_num);
        return BTreeStatus::FailedToRemove;
    }

    pager->unref_page(static_cast<int>(*ctx.left_sibling_page_num));
    pager->unref_page(underflow_page_num);
    return finish_parent_after_merge(parent_page, ctx.parent_page_num, path);
}

BTreeStatus BTree::propagate_merging(std::uint32_t underflow_page_num, std::vector<TraversalPathEntry> &path) {
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
    PagerGetResult get_underflow_result = pager->get(underflow_page_num);
    if (get_underflow_result.status != PagerResult::Success) return BTreeStatus::FailedToRead;

    PageType underflow_page_type = BTreePage::peek_page_type(get_underflow_result.data);

    // Roots are handled separately because they are allowed to break the normal
    // min-key invariant.
    if (path.empty()) {
        return handle_root_underflow(underflow_page_num, underflow_page_type, get_underflow_result.data);
    }

    // Load the parent and compute the sibling context around the underflowing page.
    TraversalPathEntry parent_path_entry = path.back();
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

    BInternalPage parent_page(get_parent_result.data);
    MergeParentContext ctx = build_merge_parent_context(parent_path_entry, parent_page);

    if (underflow_page_type == PageType::Leaf) {
        BLeafPage current_leaf(get_underflow_result.data);

        // Try to repair the leaf by borrowing before we merge anything.
        if (ctx.right_sibling_page_num != std::nullopt) {
            BTreeStatus right_borrow_result = borrow_from_right_leaf(current_leaf, parent_page, ctx, underflow_page_num);
            if (right_borrow_result == BTreeStatus::Success) return BTreeStatus::Success;
            if (right_borrow_result == BTreeStatus::FailedToRead) return right_borrow_result;
        }

        if (ctx.left_sibling_page_num != std::nullopt) {
            BTreeStatus left_borrow_result = borrow_from_left_leaf(current_leaf, parent_page, ctx, underflow_page_num);
            if (left_borrow_result == BTreeStatus::Success) return BTreeStatus::Success;
            if (left_borrow_result == BTreeStatus::FailedToRead) return left_borrow_result;
        }

        // If borrowing failed from both sides, we have to merge.
        if (ctx.right_sibling_page_num != std::nullopt) {
            return merge_with_right_leaf(current_leaf, parent_page, ctx, underflow_page_num, path);
        }
        return merge_with_left_leaf(current_leaf, parent_page, ctx, underflow_page_num, path);
    }

    BInternalPage current_page(get_underflow_result.data);

    // Same flow for internal pages: try borrowing first, then fall back to merging.
    if (ctx.right_sibling_page_num != std::nullopt) {
        BTreeStatus right_borrow_result = borrow_from_right_internal(current_page, parent_page, ctx, underflow_page_num);
        if (right_borrow_result == BTreeStatus::Success) return BTreeStatus::Success;
        if (right_borrow_result == BTreeStatus::FailedToRead) return right_borrow_result;
    }

    if (ctx.left_sibling_page_num != std::nullopt) {
        BTreeStatus left_borrow_result = borrow_from_left_internal(current_page, parent_page, ctx, underflow_page_num);
        if (left_borrow_result == BTreeStatus::Success) return BTreeStatus::Success;
        if (left_borrow_result == BTreeStatus::FailedToRead) return left_borrow_result;
    }

    if (ctx.right_sibling_page_num != std::nullopt) {
        return merge_with_right_internal(current_page, parent_page, ctx, underflow_page_num, path);
    }
    return merge_with_left_internal(current_page, parent_page, ctx, underflow_page_num, path);
}

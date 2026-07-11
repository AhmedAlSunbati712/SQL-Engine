#include <BTree.h>
#include <Pager.h>
#include <cassert>
BTree::~BTree() {
    // Ensuring the precondition is met
    assert((pager_open == false && pager == nullptr) || (pager_open && pager));
    if (pager_open) {
        delete pager;
    }
    return;
}

BTreeStatus BTree::open(std::string db_file) {
    pager = new Pager();
    PagerResult open_result = pager->open(db_file);
    if (open_result != PagerResult::Success) return BTreeStatus::FailedToOpenDB;
    return BTreeStatus::Success;
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
    return BTreeRemoveStatus{};
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
     * Purpose:
     * The page split_page_num overflowed after an insert.
     * Split it into:
     *  - the original left page
     *  - a newly allocated right sibling
     * Then repair the parent. If the parent overflows, recurse upward.
     *
     * ============================================================
     * Splitting a leaf page
     * ============================================================
     *
     * Example:
     * max keys = 4
     * overflowing leaf:
     *  [10, 20, 30, 40, 50]
     *
     * After split:
     *  left leaf  = [10, 20]
     *  right leaf = [30, 40, 50]
     *
     * For a leaf split:
     * - the promoted separator is the first key of the new right leaf
     * - that promoted key stays inside the right leaf
     * - no existing parent separator needs readjustment
     *
     * So here:
     *  promoted_key = 30
     *
     * Parent repair:
     * - if the parent currently has:
     *      keys:     [k1, k2, k3]
     *      children: [c0, c1, c2, c3]
     * - and c1 is the leaf that split
     * - then after allocating new right leaf c_new:
     *      keys:     [k1, promoted_key, k2, k3]
     *      children: [c0, c1, c_new, c2, c3]
     *
     * Special case:
     * - if the split leaf was the root and there is no parent:
     *   allocate a new internal root
     *   root.keys     = [promoted_key]
     *   root.children = [old_leaf, new_right_leaf]
     *
     * ============================================================
     * Splitting an internal page
     * ============================================================
     *
     * Example:
     * overflowing internal page:
     *  keys:     [10, 20, 30, 40, 50]
     *  children: [c0, c1, c2, c3, c4, c5]
     *
     * Split around the median:
     * - promoted_key = 30
     * - left page keeps:
     *      keys:     [10, 20]
     *      children: [c0, c1, c2]
     * - right page gets:
     *      keys:     [40, 50]
     *      children: [c3, c4, c5]
     *
     * For an internal split:
     * - the promoted median key does NOT stay in either child
     * - it moves upward into the parent only
     * - no existing parent separator needs readjustment
     *
     * Parent repair is the same pattern:
     * - replace the one child pointer to the old page with:
     *      left child  = old page
     *      separator   = promoted_key
     *      right child = new right page
     *
     * Special case:
     * - if the split internal page was the root:
     *   allocate a new internal root
     *   root.keys     = [promoted_key]
     *   root.children = [old_internal, new_right_internal]
     *
     * ============================================================
     * Recursive flow
     * ============================================================
     *
     * 1. Load split_page_num and inspect whether it is a leaf or internal page.
     * 2. Allocate a new right sibling page.
     * 3. Redistribute keys between left and right.
     *    - for a leaf: move key-value pairs
     *    - for an internal page: move keys and child page numbers
     * 4. Compute promoted_key:
     *    - leaf split: first key of the new right leaf
     *    - internal split: the median key removed from the old page
     * 5. Write back both child pages.
     * 6. If there is no parent in path:
     *    - allocate a new root
     *    - install promoted_key and the two child pointers
     *    - update the pager's root page number in the DB header
     *    - return success
     * 7. Otherwise:
     *    - load the parent page
     *    - insert promoted_key and the new right child immediately to the right of the old child
     *    - if the parent overflowed, recursively call propagate_splitting on the parent
     *    - otherwise write back the parent and return success
     * 
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

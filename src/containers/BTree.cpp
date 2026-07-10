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
    if (descent_result.status != BTreeStatus::Success) {
        get_result.status = descent_result.status;
        return get_result;
    }

    // Build the in-memory object for the leaf page
    BLeafPage target_leaf(descent_result.leaf_page);
    
    // Search for the idx of the first key that is less than or
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
     * if descent failed, return failure
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
     * first_key_changed <- (idx == 0)
     * needs_split <- (leaf.key_count() == m)
     *
     * leaf.write_back()
     *
     * if needs_split {
     *      # Recursive helper handles all parent splits and also handles the root case itself
     *      split_result <- propagate_splitting(
     *          split_page_num = descent.leaf_page_num,
     *          path = descent.path,
     *          split_page_type = leaf,
     *          split_key = leaf.first_key_of_right_split()
     *      )
     *
     *      pager->unref(descent.leaf_page_num)
     *      return split_result.status
     * }
     *
     * if first_key_changed {
     *      propagate_separator_change_upward(descent.path, leaf.first_key())
     * }
     *
     * pager->unref(descent.leaf_page_num)
     * return success
     */
    return BTreeStatus{};
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
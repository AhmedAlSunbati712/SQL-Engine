#include <BTreeCursor.h>

#include <cassert>
#include <utility>

BTreeCursor::BTreeCursor(BTree *tree) : tree(tree) {
    // A cursor can only be created by a BTree that already has an open pager.
    assert(tree != nullptr);
    assert(tree->pager_open && tree->pager != nullptr);

    // Register immediately even though the cursor starts unpositioned. This prevents
    // a write from starting between cursor construction and the first seek.
    tree->register_cursor();
}

BTreeCursorStatus BTreeCursor::release_current_leaf() {
    if (current_leaf_data == nullptr) {
        current_leaf_page_num = 0;
        current_key_idx = 0;
        path.clear();
        positioned = false;
        return BTreeCursorStatus::Success;
    }

    // Keep the cursor state unchanged when unref fails. We still have to assume
    // that this cursor owns the page reference in that case.
    PagerResult release_result = tree->pager->unref_page(current_leaf_page_num);
    if (release_result != PagerResult::Success) {
        return BTreeCursorStatus::FailedToReleasePage;
    }

    current_leaf_data = nullptr;
    current_leaf_page_num = 0;
    current_key_idx = 0;
    path.clear();
    positioned = false;
    return BTreeCursorStatus::Success;
}

BTreeCursorStatus BTreeCursor::position_on_leaf(
    char *leaf_data,
    std::uint32_t leaf_page_num,
    std::size_t key_idx,
    std::vector<TraversalPathEntry> new_path
) {
    assert(tree != nullptr && tree->pager != nullptr);

    // This helper consumes a valid incoming pager reference. A null pointer means
    // that the caller never obtained such a reference, so there is nothing to release.
    if (leaf_data == nullptr) return BTreeCursorStatus::FailedToRead;

    if (closed) {
        PagerResult release_result = tree->pager->unref_page(leaf_page_num);
        if (release_result != PagerResult::Success) {
            return BTreeCursorStatus::FailedToReleasePage;
        }
        return BTreeCursorStatus::Closed;
    }

    // If the fetched page is not a leaf, release the incoming reference before
    // reporting the malformed traversal result.
    if (BTreePage::peek_page_type(leaf_data) != PageType::Leaf) {
        PagerResult release_result = tree->pager->unref_page(leaf_page_num);
        if (release_result != PagerResult::Success) {
            return BTreeCursorStatus::FailedToReleasePage;
        }
        return BTreeCursorStatus::FailedToRead;
    }

    BLeafPage leaf(leaf_data);
    if (key_idx >= leaf.get_key_count()) {
        PagerResult release_result = tree->pager->unref_page(leaf_page_num);
        if (release_result != PagerResult::Success) {
            return BTreeCursorStatus::FailedToReleasePage;
        }
        return BTreeCursorStatus::EndOfTree;
    }

    // Do not abandon the old leaf until the incoming leaf has been validated.
    BTreeCursorStatus release_result = release_current_leaf();
    if (release_result != BTreeCursorStatus::Success) {
        PagerResult incoming_release_result = tree->pager->unref_page(leaf_page_num);
        if (incoming_release_result != PagerResult::Success) {
            return BTreeCursorStatus::FailedToReleasePage;
        }
        return release_result;
    }

    current_leaf_data = leaf_data;
    current_leaf_page_num = leaf_page_num;
    current_key_idx = key_idx;
    path = std::move(new_path);
    positioned = true;
    return BTreeCursorStatus::Success;
}

BTreeCursorStatus BTreeCursor::move_to_next_leaf() {
    if (closed) return BTreeCursorStatus::Closed;
    if (!positioned) return BTreeCursorStatus::NotPositioned;

    // Work on a copy. The cursor keeps its current path and pinned leaf until a
    // complete path to the next leaf has been found.
    std::vector<TraversalPathEntry> candidate_path = path;

    while (!candidate_path.empty()) {
        TraversalPathEntry entry = candidate_path.back();
        candidate_path.pop_back();

        PagerGetResult parent_result = tree->pager->get(entry.parent_page_num);
        if (parent_result.status != PagerResult::Success) {
            return BTreeCursorStatus::FailedToRead;
        }

        // Every page recorded in a traversal path must be an internal page.
        if (BTreePage::peek_page_type(parent_result.data) != PageType::Internal) {
            PagerResult release_result = tree->pager->unref_page(entry.parent_page_num);
            if (release_result != PagerResult::Success) {
                return BTreeCursorStatus::FailedToReleasePage;
            }
            return BTreeCursorStatus::FailedToRead;
        }

        BInternalPage parent(parent_result.data);
        std::size_t child_idx = entry.separator_index_used;
        if (entry.child_dir == ChildDirection::Right) child_idx++;

        // A child index greater than key_count cannot describe a valid path through
        // an internal page with key_count + 1 children.
        if (child_idx > parent.get_key_count()) {
            PagerResult release_result = tree->pager->unref_page(entry.parent_page_num);
            if (release_result != PagerResult::Success) {
                return BTreeCursorStatus::FailedToReleasePage;
            }
            return BTreeCursorStatus::FailedToRead;
        }

        if (child_idx < parent.get_key_count()) {
            // The next subtree is the child immediately to the right of the child
            // used by the old path.
            std::optional<std::uint32_t> next_subtree_page_num = parent.get_right_child(child_idx);
            if (next_subtree_page_num == std::nullopt) {
                PagerResult release_result = tree->pager->unref_page(entry.parent_page_num);
                if (release_result != PagerResult::Success) {
                    return BTreeCursorStatus::FailedToReleasePage;
                }
                return BTreeCursorStatus::FailedToRead;
            }

            candidate_path.push_back({
                entry.parent_page_num,
                child_idx,
                ChildDirection::Right
            });

            PagerResult release_result = tree->pager->unref_page(entry.parent_page_num);
            if (release_result != PagerResult::Success) {
                return BTreeCursorStatus::FailedToReleasePage;
            }

            return descend_to_leftmost_leaf(*next_subtree_page_num, candidate_path);
        }

        // This parent has no unvisited child to the right. Release it and continue
        // upward until we find an ancestor that does.
        PagerResult release_result = tree->pager->unref_page(entry.parent_page_num);
        if (release_result != PagerResult::Success) {
            return BTreeCursorStatus::FailedToReleasePage;
        }
    }

    // No ancestor had another child to the right, so the old leaf contained the
    // final key in the tree.
    BTreeCursorStatus release_result = release_current_leaf();
    if (release_result != BTreeCursorStatus::Success) return release_result;
    return BTreeCursorStatus::EndOfTree;
}

BTreeCursorStatus BTreeCursor::descend_to_leftmost_leaf(
    std::uint32_t subtree_page_num,
    std::vector<TraversalPathEntry> &new_path
) {
    if (closed) return BTreeCursorStatus::Closed;

    // Roll the caller's path back to this point if any part of the descent fails.
    std::size_t initial_path_size = new_path.size();
    std::uint32_t current_page_num = subtree_page_num;
    PagerGetResult current_result = tree->pager->get(current_page_num);
    if (current_result.status != PagerResult::Success) {
        return BTreeCursorStatus::FailedToRead;
    }

    while (BTreePage::peek_page_type(current_result.data) == PageType::Internal) {
        BInternalPage current_page(current_result.data);
        std::optional<std::uint32_t> leftmost_child = current_page.get_leftmost_child();

        if (leftmost_child == std::nullopt) {
            PagerResult release_result = tree->pager->unref_page(current_page_num);
            new_path.resize(initial_path_size);
            if (release_result != PagerResult::Success) {
                return BTreeCursorStatus::FailedToReleasePage;
            }
            return BTreeCursorStatus::FailedToRead;
        }

        PagerGetResult child_result = tree->pager->get(*leftmost_child);
        if (child_result.status != PagerResult::Success) {
            PagerResult release_result = tree->pager->unref_page(current_page_num);
            new_path.resize(initial_path_size);
            if (release_result != PagerResult::Success) {
                return BTreeCursorStatus::FailedToReleasePage;
            }
            return BTreeCursorStatus::FailedToRead;
        }

        // Release the parent before continuing. If that fails, also release the
        // child reference we just acquired so this traversal does not leak it.
        PagerResult parent_release_result = tree->pager->unref_page(current_page_num);
        if (parent_release_result != PagerResult::Success) {
            tree->pager->unref_page(*leftmost_child);
            new_path.resize(initial_path_size);
            return BTreeCursorStatus::FailedToReleasePage;
        }

        // Taking the leftmost child is represented relative to separator zero.
        new_path.push_back({
            current_page_num,
            0,
            ChildDirection::Left
        });

        current_page_num = *leftmost_child;
        current_result = child_result;
    }

    BLeafPage leaf(current_result.data);
    if (leaf.get_key_count() == 0) {
        PagerResult release_result = tree->pager->unref_page(current_page_num);
        new_path.resize(initial_path_size);
        if (release_result != PagerResult::Success) {
            return BTreeCursorStatus::FailedToReleasePage;
        }
        return BTreeCursorStatus::EndOfTree;
    }

    return position_on_leaf(
        current_result.data,
        current_page_num,
        0,
        std::move(new_path)
    );
}

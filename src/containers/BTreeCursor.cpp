#include <BTreeCursor.h>

#include <cassert>
#include <cstdlib>
#include <utility>

BTreeCursor::BTreeCursor(BTree *tree) : tree(tree) {
    // A cursor can only be created by a BTree that already has an open pager.
    assert(tree != nullptr);
    assert(tree->pager_open && tree->pager != nullptr);

    // Register immediately even though the cursor starts unpositioned. This prevents
    // a write from starting between cursor construction and the first seek.
    tree->register_cursor();
}

BTreeCursor::~BTreeCursor() {
    if (closed) return;

    // A destructor cannot report that it failed to release ownership. If that ever
    // happens, the pager reference count can no longer be trusted.
    if (close() != BTreeCursorStatus::Success) std::abort();
}

BTreeCursor::BTreeCursor(BTreeCursor &&other) noexcept
    : tree(other.tree),
      current_leaf_data(other.current_leaf_data),
      current_leaf_page_num(other.current_leaf_page_num),
      current_key_idx(other.current_key_idx),
      path(std::move(other.path)),
      positioned(other.positioned),
      exhausted(other.exhausted),
      closed(other.closed) {
    // The registration belongs to this object now. Make the moved-from cursor
    // closed so its destructor does not release the page or unregister again.
    other.tree = nullptr;
    other.current_leaf_data = nullptr;
    other.current_leaf_page_num = 0;
    other.current_key_idx = 0;
    other.path.clear();
    other.positioned = false;
    other.exhausted = false;
    other.closed = true;
}

BTreeCursor &BTreeCursor::operator=(BTreeCursor &&other) noexcept {
    if (this == &other) return *this;

    // Drop this cursor's existing registration before taking ownership of the other
    // one. Cleanup failure is fatal because move assignment cannot return a status.
    if (!closed && close() != BTreeCursorStatus::Success) std::abort();

    tree = other.tree;
    current_leaf_data = other.current_leaf_data;
    current_leaf_page_num = other.current_leaf_page_num;
    current_key_idx = other.current_key_idx;
    path = std::move(other.path);
    positioned = other.positioned;
    exhausted = other.exhausted;
    closed = other.closed;

    other.tree = nullptr;
    other.current_leaf_data = nullptr;
    other.current_leaf_page_num = 0;
    other.current_key_idx = 0;
    other.path.clear();
    other.positioned = false;
    other.exhausted = false;
    other.closed = true;
    return *this;
}

BTreeCursorStatus BTreeCursor::seek_first() {
    if (closed) return BTreeCursorStatus::Closed;

    BTreeCursorStatus release_result = release_current_leaf();
    if (release_result != BTreeCursorStatus::Success) return release_result;
    exhausted = false;

    PagerGetRootResult root_result = tree->pager->get_btree_root();
    if (root_result.status == PagerResult::EmptyBTree) {
        exhausted = true;
        return BTreeCursorStatus::EndOfTree;
    }
    if (root_result.status != PagerResult::Success) {
        return BTreeCursorStatus::FailedToRead;
    }

    std::vector<TraversalPathEntry> new_path;
    if (BTreePage::peek_page_type(root_result.data) == PageType::Leaf) {
        BTreeCursorStatus position_result = position_on_leaf(
            root_result.data,
            root_result.root_page_num,
            0,
            std::move(new_path)
        );
        if (position_result == BTreeCursorStatus::EndOfTree) exhausted = true;
        return position_result;
    }

    // Keep the first root reference pinned while the helper acquires its own traversal
    // reference. This prevents another process from changing the tree between the two.
    BTreeCursorStatus descent_result = descend_to_leftmost_leaf(
        root_result.root_page_num,
        new_path
    );

    PagerResult root_release_result = tree->pager->unref_page(root_result.root_page_num);
    if (root_release_result != PagerResult::Success) {
        if (release_current_leaf() != BTreeCursorStatus::Success) std::abort();
        exhausted = false;
        return BTreeCursorStatus::FailedToReleasePage;
    }

    if (descent_result == BTreeCursorStatus::EndOfTree) exhausted = true;
    return descent_result;
}

BTreeCursorStatus BTreeCursor::seek(const Key &target) {
    if (closed) return BTreeCursorStatus::Closed;

    // Seeking starts a new position. A failed seek intentionally leaves this cursor
    // unpositioned instead of restoring its previous leaf and path.
    BTreeCursorStatus release_result = release_current_leaf();
    if (release_result != BTreeCursorStatus::Success) return release_result;
    exhausted = false;

    LeafDescentResult descent_result = tree->descend_from_root_to_leaf(target, true);
    if (descent_result.status == BTreeStatus::EmptyTree) {
        exhausted = true;
        return BTreeCursorStatus::EndOfTree;
    }
    if (descent_result.status != BTreeStatus::Success) {
        return BTreeCursorStatus::FailedToRead;
    }

    BLeafPage leaf(descent_result.leaf_page);
    std::size_t key_idx = leaf.lower_bound_key(target);
    if (key_idx < leaf.get_key_count()) {
        return position_on_leaf(
            descent_result.leaf_page,
            descent_result.leaf_page_num,
            key_idx,
            std::move(descent_result.path)
        );
    }

    if (leaf.get_key_count() == 0) {
        PagerResult leaf_release_result = tree->pager->unref_page(descent_result.leaf_page_num);
        if (leaf_release_result != PagerResult::Success) {
            return BTreeCursorStatus::FailedToReleasePage;
        }
        exhausted = true;
        return BTreeCursorStatus::EndOfTree;
    }

    // The target belongs after every key in this leaf. Install its final key only as
    // a traversal anchor, then cross the boundary to the next leaf if one exists.
    BTreeCursorStatus anchor_result = position_on_leaf(
        descent_result.leaf_page,
        descent_result.leaf_page_num,
        leaf.get_key_count() - 1,
        std::move(descent_result.path)
    );
    if (anchor_result != BTreeCursorStatus::Success) return anchor_result;

    BTreeCursorStatus move_result = move_to_next_leaf();
    if (move_result == BTreeCursorStatus::EndOfTree) {
        exhausted = true;
        return move_result;
    }
    if (move_result != BTreeCursorStatus::Success) {
        if (release_current_leaf() != BTreeCursorStatus::Success) {
            return BTreeCursorStatus::FailedToReleasePage;
        }
    }
    return move_result;
}

BTreeCursorStatus BTreeCursor::next() {
    if (closed) return BTreeCursorStatus::Closed;
    if (exhausted) return BTreeCursorStatus::EndOfTree;
    if (!positioned) return BTreeCursorStatus::NotPositioned;

    BLeafPage leaf(current_leaf_data);
    if (current_key_idx + 1 < leaf.get_key_count()) {
        current_key_idx++;
        return BTreeCursorStatus::Success;
    }

    BTreeCursorStatus move_result = move_to_next_leaf();
    if (move_result == BTreeCursorStatus::EndOfTree) exhausted = true;
    return move_result;
}

BTreeCursorResult BTreeCursor::current() const {
    BTreeCursorResult result{};
    if (closed) {
        result.status = BTreeCursorStatus::Closed;
        return result;
    }
    if (exhausted) {
        result.status = BTreeCursorStatus::EndOfTree;
        return result;
    }
    if (!positioned || current_leaf_data == nullptr) {
        result.status = BTreeCursorStatus::NotPositioned;
        return result;
    }

    BLeafPage leaf(current_leaf_data);
    std::optional<Key> key = leaf.key_at(current_key_idx);
    std::optional<Value> value = leaf.get_at(current_key_idx);
    if (key == std::nullopt || value == std::nullopt) {
        result.status = BTreeCursorStatus::FailedToRead;
        return result;
    }

    result.status = BTreeCursorStatus::Success;
    result.key = std::move(*key);
    result.value = std::move(*value);
    return result;
}

bool BTreeCursor::valid() const {
    return !closed && !exhausted && positioned && current_leaf_data != nullptr;
}

BTreeCursorStatus BTreeCursor::close() {
    if (closed) return BTreeCursorStatus::Success;

    BTreeCursorStatus release_result = release_current_leaf();
    if (release_result != BTreeCursorStatus::Success) return release_result;

    assert(tree != nullptr);
    tree->unregister_cursor();
    tree = nullptr;
    exhausted = false;
    closed = true;
    return BTreeCursorStatus::Success;
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

#include <BTreeCursor.h>

BTreeCursor::BTreeCursor(BTree *tree) {
    /**
     * assert tree is not null
     * assert tree has an open pager
     *
     * this->tree <- tree
     * positioned <- false
     * closed <- false
     *
     * Tell the BTree that it now has one active read cursor. While this count is
     * greater than zero, the same BTree object must reject insert, remove, commit,
     * rollback, and close because any of them could invalidate this cursor's path.
     *
     * The constructor does not seek anywhere. The client still has to call
     * seek_first() or seek(key) before current() and next() can be used.
     */
}

BTreeCursorStatus BTreeCursor::release_current_leaf() {
    /**
     * If current_leaf_data is null:
     *      clear the current position and path
     *      return Success
     *
     * release_result <- tree->pager->unref_page(current_leaf_page_num)
     * if release_result failed:
     *      Keep all cursor fields unchanged because we still have to assume that
     *      the cursor owns the page reference.
     *      return FailedToReleasePage
     *
     * current_leaf_data <- null
     * current_leaf_page_num <- 0
     * current_key_idx <- 0
     * path.clear()
     * positioned <- false
     * return Success
     */
}

BTreeCursorStatus BTreeCursor::position_on_leaf(
    char *leaf_data,
    std::uint32_t leaf_page_num,
    std::size_t key_idx,
    std::vector<TraversalPathEntry> new_path
) {
    /**
     * This helper consumes the pager reference represented by leaf_data. If it
     * succeeds, the cursor owns that reference. If it fails, it must release it.
     *
     * if leaf_data is null or it does not describe a leaf page:
     *      unref leaf_page_num if needed
     *      return FailedToRead
     *
     * leaf <- decode leaf_data
     * if key_idx is outside the leaf:
     *      unref leaf_page_num
     *      return EndOfTree
     *
     * release_result <- release_current_leaf()
     * if release_result failed:
     *      unref leaf_page_num because the cursor cannot take ownership of it
     *      return release_result
     *
     * current_leaf_data <- leaf_data
     * current_leaf_page_num <- leaf_page_num
     * current_key_idx <- key_idx
     * path <- move(new_path)
     * positioned <- true
     * return Success
     */
}

BTreeCursorStatus BTreeCursor::move_to_next_leaf() {
    /**
     * We only call this after current_key_idx reaches the end of the current leaf.
     * Keep the current leaf pinned until the next leaf has been found successfully.
     *
     * if the cursor is closed:
     *      return Closed
     * if the cursor is not positioned:
     *      return NotPositioned
     *
     * candidate_path <- path
     *
     * while candidate_path is not empty:
     *      entry <- candidate_path.back()
     *      candidate_path.pop_back()
     *
     *      parent_result <- tree->pager->get(entry.parent_page_num)
     *      if parent_result failed:
     *          return FailedToRead
     *
     *      parent <- decode parent_result.data as an internal page
     *
     *      Convert the separator information back into the child index that was
     *      taken during descent:
     *          if entry.child_dir is Left:
     *              child_idx <- entry.separator_index_used
     *          otherwise:
     *              child_idx <- entry.separator_index_used + 1
     *
     *      if child_idx is less than parent.key_count:
     *          There is another child immediately to the right of the old path.
     *          next_subtree_page_num <- parent.get_right_child(child_idx)
     *
     *          Add the step from this parent into that right child:
     *              candidate_path.push_back({
     *                  entry.parent_page_num,
     *                  child_idx,
     *                  ChildDirection::Right
     *              })
     *
     *          unref the parent page
     *          if unref failed:
     *              return FailedToReleasePage
     *
     *          descend_to_leftmost_leaf() appends the rest of the path directly to
     *          candidate_path. The cursor's installed path is not touched until the
     *          new leaf has been reached successfully.
     *          return descend_to_leftmost_leaf(
     *              next_subtree_page_num,
     *              candidate_path
     *          )
     *
     *      unref the parent page
     *      if unref failed:
     *          return FailedToReleasePage
     *
     *      This parent had no unvisited child to the right, so continue upward.
     *
     * No ancestor had another child to the right. The current key was the final
     * key in the tree.
     * release_result <- release_current_leaf()
     * if release_result failed:
     *      return release_result
     * return EndOfTree
     */
}

BTreeCursorStatus BTreeCursor::descend_to_leftmost_leaf(
    std::uint32_t subtree_page_num,
    std::vector<TraversalPathEntry> &new_path
) {
    /**
     * new_path already contains the path prefix leading to subtree_page_num. Append
     * the rest of the leftmost descent to it. Do not replace the cursor position
     * until the complete descent succeeds.
     *
     * initial_path_size <- new_path.size()
     * current_page_num <- subtree_page_num
     * current_result <- tree->pager->get(current_page_num)
     * if current_result failed:
     *      new_path.resize(initial_path_size)
     *      return FailedToRead
     *
     * while current_result.data describes an internal page:
     *      current_page <- decode current_result.data as an internal page
     *      leftmost_child <- current_page.get_leftmost_child()
     *      if leftmost_child does not exist:
     *          unref current_page_num
     *          new_path.resize(initial_path_size)
     *          return FailedToRead
     *
     *      child_result <- tree->pager->get(leftmost_child)
     *      if child_result failed:
     *          unref current_page_num
     *          new_path.resize(initial_path_size)
     *          return FailedToRead
     *
     *      Since we always take the leftmost child, record the traversal relative
     *      to separator zero.
     *      new_path.push_back({
     *          current_page_num,
     *          0,
     *          ChildDirection::Left
     *      })
     *
     *      unref current_page_num
     *      if unref failed:
     *          unref leftmost_child so its new reference is not leaked
     *          new_path.resize(initial_path_size)
     *          return FailedToReleasePage
     *
     *      current_page_num <- leftmost_child
     *      current_result <- child_result
     *
     * leaf <- decode current_result.data as a leaf page
     * if leaf is empty:
     *      unref current_page_num
     *      new_path.resize(initial_path_size)
     *      return EndOfTree
     *
     * return position_on_leaf(
     *      current_result.data,
     *      current_page_num,
     *      0,
     *      move(new_path)
     * )
     */
}

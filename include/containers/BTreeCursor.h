#pragma once

#include <BTree.h>

#include <cstddef>
#include <cstdint>
#include <vector>

enum class BTreeCursorStatus : std::uint8_t {
    Success = 0,
    EndOfTree,
    NotPositioned,
    Closed,
    FailedToRead,
    FailedToReleasePage
};

struct BTreeCursorResult {
    BTreeCursorStatus status = BTreeCursorStatus::NotPositioned;
    Key key;
    Value value;
};

class BTreeCursor {
    public:
        ~BTreeCursor();

        // A cursor owns the reference to its current leaf page, so copying it would
        // make it unclear which object is responsible for releasing that reference.
        BTreeCursor(const BTreeCursor &) = delete;
        BTreeCursor &operator=(const BTreeCursor &) = delete;
        BTreeCursor(BTreeCursor &&other) noexcept;
        BTreeCursor &operator=(BTreeCursor &&other) noexcept;

        // Position on the smallest key in the tree.
        BTreeCursorStatus seek_first();

        // Position on the first key greater than or equal to target.
        BTreeCursorStatus seek(const Key &target);

        // Move to the next key according to KeyCodec ordering.
        BTreeCursorStatus next();

        // Return a copy of the key-value pair at the current position.
        BTreeCursorResult current() const;

        bool valid() const;
        BTreeCursorStatus close();

    private:
        friend class BTree;

        explicit BTreeCursor(BTree *tree);

        BTreeCursorStatus release_current_leaf();
        BTreeCursorStatus position_on_leaf(
            char *leaf_data,
            std::uint32_t leaf_page_num,
            std::size_t key_idx,
            std::vector<TraversalPathEntry> new_path
        );
        BTreeCursorStatus move_to_next_leaf();
        BTreeCursorStatus descend_to_leftmost_leaf(std::uint32_t subtree_page_num);

        // The BTree must outlive its cursors. The cursor keeps only its current leaf
        // pinned; internal pages are loaded and released while crossing leaf boundaries.
        BTree *tree = nullptr;
        char *current_leaf_data = nullptr;
        std::uint32_t current_leaf_page_num = 0;
        std::size_t current_key_idx = 0;
        std::vector<TraversalPathEntry> path;
        bool positioned = false;
        bool closed = false;
};

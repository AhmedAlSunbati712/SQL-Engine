#pragma once
#include <BTreePage.h>
#include <Pager.h>
#include <cstddef>
#include <string>
#include <vector>

constexpr std::size_t BTREE_ORDER = 200;
constexpr std::size_t MAX_KEYS(std::size_t order) { return order - 1; }
constexpr std::size_t MIN_KEYS(std::size_t order) { return ((order + 1) / 2) - 1; }

enum class BTreeStatus : std::uint8_t {
    Success = 0,
    FailedToOpenDB,
    FailedToCloseDB,
    KeyNotInTree,
    EmptyTree,
    FailedToGetRoot,
    FailedToRead,
    FailedToInsert,
    FailedToRemove,
    FailedToAllocateNewPage,
    CursorActive,
};

enum class BTreeCommitStatus : std::uint8_t {
    Success = 0,
    Failed,
    CursorActive
};

enum class BTreeRollbackStatus : std::uint8_t {
    Success = 0,
    Failed,
    CursorActive
};

enum class ChildDirection : std::uint8_t {
    Right = 0,
    Left
};

struct BTreeGetStatus {
    BTreeStatus status;
    Value value;
};

struct BTreeRemoveStatus {
    BTreeStatus status = BTreeStatus::Success;
    Value value;
};

struct TraversalPathEntry {
    std::uint32_t parent_page_num;
    std::size_t separator_index_used;
    ChildDirection child_dir;
};

struct LeafDescentResult {
    BTreeStatus status = BTreeStatus::Success;
    char *leaf_page = nullptr;
    std::uint32_t leaf_page_num = 0;
    std::vector<TraversalPathEntry> path;
};

class BTreeCursor;

class BTree {
    public:
        BTree() = default;
        ~BTree();
        BTreeStatus open(std::string db_file);
        BTreeStatus close();
        BTreeCommitStatus commit();
        BTreeRollbackStatus rollback();
        BTreeGetStatus get(const Key &key);
        BTreeStatus insert(const Key &key, Value &value);
        BTreeRemoveStatus remove(const Key &key);
        // The database must already be open. The returned cursor starts unpositioned.
        BTreeCursor open_cursor();
    private:
        friend class BTreeCursor;

        void register_cursor();
        void unregister_cursor();
        LeafDescentResult descend_from_root_to_leaf(const Key &key, bool include_path);
        struct MergeParentContext {
            std::uint32_t parent_page_num;
            std::size_t child_idx;
            std::optional<std::uint32_t> right_sibling_page_num;
            std::optional<std::uint32_t> left_sibling_page_num;
        };

        BTreeStatus propagate_separator_change_upward(const std::vector<TraversalPathEntry> &path, const Key &new_subtree_min);
        BTreeStatus propagate_splitting(std::uint32_t split_page_num, std::vector<TraversalPathEntry> &path);
        BTreeStatus propagate_merging(std::uint32_t underflow_page_num, std::vector<TraversalPathEntry> &path);
        BTreeStatus handle_splitting_root(std::uint32_t left_child_page_num, std::uint32_t right_child_page_num, const Key &separator_key);
        BTreeStatus handle_root_underflow(std::uint32_t underflow_page_num, PageType underflow_page_type, char *underflow_page_data);
        MergeParentContext build_merge_parent_context(const TraversalPathEntry &parent_path_entry, BInternalPage &parent_page);
        BTreeStatus finish_parent_after_merge(BInternalPage &parent_page, std::uint32_t parent_page_num, std::vector<TraversalPathEntry> &path);

        BTreeStatus borrow_from_right_leaf(BLeafPage &current_leaf, BInternalPage &parent_page, const MergeParentContext &ctx, std::uint32_t underflow_page_num);
        BTreeStatus borrow_from_left_leaf(BLeafPage &current_leaf, BInternalPage &parent_page, const MergeParentContext &ctx, std::uint32_t underflow_page_num);
        BTreeStatus merge_with_right_leaf(BLeafPage &current_leaf, BInternalPage &parent_page, const MergeParentContext &ctx, std::uint32_t underflow_page_num, std::vector<TraversalPathEntry> &path);
        BTreeStatus merge_with_left_leaf(BLeafPage &current_leaf, BInternalPage &parent_page, const MergeParentContext &ctx, std::uint32_t underflow_page_num, std::vector<TraversalPathEntry> &path);

        BTreeStatus borrow_from_right_internal(BInternalPage &current_page, BInternalPage &parent_page, const MergeParentContext &ctx, std::uint32_t underflow_page_num);
        BTreeStatus borrow_from_left_internal(BInternalPage &current_page, BInternalPage &parent_page, const MergeParentContext &ctx, std::uint32_t underflow_page_num);
        BTreeStatus merge_with_right_internal(BInternalPage &current_page, BInternalPage &parent_page, const MergeParentContext &ctx, std::uint32_t underflow_page_num, std::vector<TraversalPathEntry> &path);
        BTreeStatus merge_with_left_internal(BInternalPage &current_page, BInternalPage &parent_page, const MergeParentContext &ctx, std::uint32_t underflow_page_num, std::vector<TraversalPathEntry> &path);

        static void migrate_leaf(BLeafPage &src, BLeafPage &dst, std::size_t separator_idx);
        static void migrate_internal(BInternalPage &src, BInternalPage &dst, std::size_t median_separator_idx);
        Pager *pager = nullptr;
        std::string currently_open_db_file;
        // Writes, transaction boundaries, and close are rejected while this is nonzero.
        std::size_t active_cursor_count = 0;
        bool pager_open = false;
};

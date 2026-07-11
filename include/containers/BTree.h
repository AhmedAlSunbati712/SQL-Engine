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
};

enum class BTreeCommitStatus : std::uint8_t {
    Success = 0,
    Failed
};

enum class BTreeRollbackStatus : std::uint8_t {
    Success = 0,
    Failed
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
    BTreeStatus status;
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


class BTree {
    public:
        BTree() = default;
        ~BTree();
        BTreeStatus open(std::string db_file);
        BTreeCommitStatus commit();
        BTreeRollbackStatus rollback();
        BTreeGetStatus get(std::uint64_t key);
        BTreeStatus insert(std::uint64_t key, Value &value);
        BTreeRemoveStatus remove(std::uint64_t key);
    private:
        LeafDescentResult descend_from_root_to_leaf(std::uint64_t key, bool include_path);
        std::size_t minimum_allowed_keys() const;
        BTreeStatus propagate_separator_change_upward(const std::vector<TraversalPathEntry> &path, std::uint64_t new_subtree_min);
        BTreeStatus propagate_splitting(std::uint32_t split_page_num, std::vector<TraversalPathEntry> &path);
        BTreeStatus propagate_merging(std::uint32_t underflow_page_num, std::vector<TraversalPathEntry> &path);
        BTreeStatus handle_splitting_root(std::uint32_t left_child_page_num, std::uint32_t right_child_page_num, std::uint64_t separator_key);

        static void migrate_leaf(BLeafPage src, BLeafPage dst, std::size_t separator_idx);
        static void migrate_internal(BInternalPage src, BInternalPage dst, std::size_t median_separator_idx);
        Pager *pager = nullptr;
        std::string currently_open_db_file;
        bool pager_open = false;
};

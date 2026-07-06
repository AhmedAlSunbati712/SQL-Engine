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
    FailedToInsert,
    FailedToRemove,
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
    std::size_t child_index_taken;
    std::size_t separator_index_used;
};

struct LeafDescentResult {
    BTreeStatus status;
    Page *leaf_page = nullptr;
    std::uint32_t leaf_page_num = 0;
    std::vector<TraversalPathEntry> path;
};

class BTree {
    public:
        BTree() = default;
        ~BTree();
        BTreeStatus open(std::string db_file);
        BTreeStatus close();
        BTreeGetStatus get(std::uint64_t key);
        BTreeStatus insert(std::uint64_t key, Value &value);
        BTreeRemoveStatus remove(std::uint64_t key);
    private:
        LeafDescentResult descend_from_root_to_leaf(std::uint64_t key);
        std::size_t minimum_allowed_keys() const;
        BTreeStatus propagate_separator_change_upward(const std::vector<TraversalPathEntry> &path, std::uint64_t new_subtree_min);
        BTreeStatus propagate_splitting(std::uint32_t split_page_num, const std::vector<TraversalPathEntry> &path, std::uint64_t split_key);
        BTreeStatus propagate_merging(std::uint32_t underflow_page_num, const std::vector<TraversalPathEntry> &path);

        Pager *pager = nullptr;
        std::string currently_open_db_file;
        bool pager_open = false;
};

#pragma once
#include <Transaction.h>

#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

class WaitForGraph {
    public:
        explicit WaitForGraph() = default;
        ~WaitForGraph() = default;

        WaitForGraph(const WaitForGraph&) = delete;
        WaitForGraph& operator=(const WaitForGraph&) = delete;
        WaitForGraph(WaitForGraph&&) = delete;
        WaitForGraph& operator=(WaitForGraph&&) = delete;

        void add_node(TransactionId txn_id);
        bool exists(TransactionId txn_id) const;
        void remove(TransactionId txn_id);

        // Returns false when either node is missing or the edge would create
        // a cycle. Duplicate edges are successful no-ops.
        bool add_edge(TransactionId from, TransactionId to);
        void remove_edge(TransactionId from, TransactionId to);

    private:
        // The caller must hold mutex_ for the complete traversal.
        bool path_exists(TransactionId from, TransactionId to) const;

        std::unordered_map<TransactionId, std::unordered_set<TransactionId>> outgoing_links_;
        std::unordered_map<TransactionId, std::unordered_set<TransactionId>> incoming_links_;
        mutable std::shared_mutex mutex_;
};

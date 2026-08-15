#pragma once
#include <TransactionManager/Transaction.h>

#include <shared_mutex>
#include <span>
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

        // Validates and adds the complete edge set atomically. If any edge is
        // invalid, none of the new edges are retained.
        bool add_edges(
            TransactionId from,
            std::span<const TransactionId> destinations);

        void remove_edge(TransactionId from, TransactionId to);

        // Removes the dependencies created by one pending lock request while
        // preserving transactions that are waiting for this transaction.
        void remove_outgoing(TransactionId txn_id);

    private:
        // The caller must hold mutex_ for the complete traversal.
        bool path_exists(TransactionId from, TransactionId to) const;

        std::unordered_map<TransactionId, std::unordered_set<TransactionId>> outgoing_links_;
        std::unordered_map<TransactionId, std::unordered_set<TransactionId>> incoming_links_;
        mutable std::shared_mutex mutex_;
};

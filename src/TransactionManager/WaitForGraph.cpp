#include <WaitForGraph.h>

#include <mutex>
#include <queue>
#include <vector>

bool WaitForGraph::exists(TransactionId txn_id) const {
    std::shared_lock lock(mutex_);
    return outgoing_links_.find(txn_id) != outgoing_links_.end();
}

void WaitForGraph::add_node(TransactionId txn_id) {
    std::unique_lock lock(mutex_);
    if (outgoing_links_.find(txn_id) != outgoing_links_.end()) return;

    // Keep both indexes present for every transaction, including a
    // transaction that currently has no edges.
    outgoing_links_.emplace(txn_id, std::unordered_set<TransactionId>{});
    try {
        incoming_links_.emplace(txn_id, std::unordered_set<TransactionId>{});
    } catch (...) {
        outgoing_links_.erase(txn_id);
        throw;
    }
}

void WaitForGraph::remove(TransactionId txn_id) {
    std::unique_lock lock(mutex_);
    auto outgoing = outgoing_links_.find(txn_id);
    auto incoming = incoming_links_.find(txn_id);
    if (outgoing == outgoing_links_.end() || incoming == incoming_links_.end()) return;

    // Remove this transaction from every destination's incoming index.
    for (TransactionId destination : outgoing->second) {
        auto destination_links = incoming_links_.find(destination);
        if (destination_links != incoming_links_.end()) {
            destination_links->second.erase(txn_id);
        }
    }

    // Remove this transaction from every source's outgoing index.
    for (TransactionId source : incoming->second) {
        auto source_links = outgoing_links_.find(source);
        if (source_links != outgoing_links_.end()) {
            source_links->second.erase(txn_id);
        }
    }

    outgoing_links_.erase(outgoing);
    incoming_links_.erase(incoming);
}

bool WaitForGraph::add_edge(TransactionId from, TransactionId to) {
    return add_edges(from, std::span<const TransactionId>(&to, 1));
}

bool WaitForGraph::add_edges(TransactionId from, std::span<const TransactionId> destinations) {
    std::unique_lock lock(mutex_);
    auto source = outgoing_links_.find(from);
    if (source == outgoing_links_.end()) return false;

    // Validate the complete request before changing either adjacency index.
    // Adding from -> to creates a cycle exactly when to can reach from.
    for (TransactionId destination : destinations) {
        if (incoming_links_.find(destination) == incoming_links_.end()) return false;
        if (path_exists(destination, from)) return false;
    }

    std::vector<TransactionId> inserted_destinations;
    inserted_destinations.reserve(destinations.size());

    try {
        for (TransactionId destination : destinations) {
            auto [_, inserted] = source->second.insert(destination);
            if (!inserted) continue;

            // Keep both indexes consistent if the reverse insertion fails.
            try {
                incoming_links_.at(destination).insert(from);
            } catch (...) {
                source->second.erase(destination);
                throw;
            }
            inserted_destinations.push_back(destination);
        }
    } catch (...) {
        // Restore the graph to its state before this batch began.
        for (TransactionId destination : inserted_destinations) {
            source->second.erase(destination);
            incoming_links_.at(destination).erase(from);
        }
        throw;
    }

    return true;
}

void WaitForGraph::remove_edge(TransactionId from, TransactionId to) {
    std::unique_lock lock(mutex_);
    auto source = outgoing_links_.find(from);
    auto destination = incoming_links_.find(to);
    if (source == outgoing_links_.end() || destination == incoming_links_.end()) return;

    source->second.erase(to);
    destination->second.erase(from);
}

void WaitForGraph::remove_outgoing(TransactionId txn_id) {
    std::unique_lock lock(mutex_);
    auto source = outgoing_links_.find(txn_id);
    if (source == outgoing_links_.end()) return;

    // Remove the reverse index entry for every dependency owned by this
    // transaction, then leave the transaction node itself in the graph.
    for (TransactionId destination : source->second) {
        auto destination_links = incoming_links_.find(destination);
        if (destination_links != incoming_links_.end()) {
            destination_links->second.erase(txn_id);
        }
    }
    source->second.clear();
}

bool WaitForGraph::path_exists(TransactionId from, TransactionId to) const {
    auto start = outgoing_links_.find(from);
    if (start == outgoing_links_.end() || outgoing_links_.find(to) == outgoing_links_.end()) {
        return false;
    }
    if (from == to) return true;

    // Traverse the existing graph while add_edges holds the exclusive mutex,
    // so the reachability decision and insertion are one atomic operation.
    std::queue<TransactionId> pending;
    std::unordered_set<TransactionId> visited;
    pending.push(from);
    visited.insert(from);

    while (!pending.empty()) {
        const TransactionId current = pending.front();
        pending.pop();

        const auto current_links = outgoing_links_.find(current);
        if (current_links == outgoing_links_.end()) continue;
        for (TransactionId neighbor : current_links->second) {
            if (neighbor == to) return true;
            if (visited.insert(neighbor).second) pending.push(neighbor);
        }
    }

    return false;
}

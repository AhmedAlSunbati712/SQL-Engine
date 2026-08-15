#include <gtest/gtest.h>

#include <WaitForGraph.h>

#include <thread>
#include <vector>

TEST(WaitForGraphTest, AddsAndRemovesNodesIdempotently) {
    WaitForGraph graph;
    EXPECT_FALSE(graph.exists(1));
    graph.add_node(1);
    graph.add_node(1);
    EXPECT_TRUE(graph.exists(1));
    graph.remove(1);
    graph.remove(1);
    EXPECT_FALSE(graph.exists(1));
}

TEST(WaitForGraphTest, RequiresBothNodesBeforeAddingEdge) {
    WaitForGraph graph;
    graph.add_node(1);
    EXPECT_FALSE(graph.add_edge(1, 2));
    EXPECT_FALSE(graph.add_edge(2, 1));
}

TEST(WaitForGraphTest, RejectsSelfAndIndirectCyclesWithoutStoringThem) {
    WaitForGraph graph;
    for (TransactionId txn_id : {1, 2, 3}) graph.add_node(txn_id);

    EXPECT_FALSE(graph.add_edge(1, 1));
    EXPECT_TRUE(graph.add_edge(1, 2));
    EXPECT_TRUE(graph.add_edge(2, 3));
    EXPECT_FALSE(graph.add_edge(3, 1));

    // The rejected edge was not installed, so removing an existing edge makes
    // the formerly cyclic direction safe.
    graph.remove_edge(2, 3);
    EXPECT_TRUE(graph.add_edge(3, 1));
}

TEST(WaitForGraphTest, DuplicateEdgesAreSuccessfulNoOps) {
    WaitForGraph graph;
    graph.add_node(1);
    graph.add_node(2);
    EXPECT_TRUE(graph.add_edge(1, 2));
    EXPECT_TRUE(graph.add_edge(1, 2));
    graph.remove_edge(1, 2);
    EXPECT_TRUE(graph.add_edge(2, 1));
}

TEST(WaitForGraphTest, AddsCompleteEdgeSetAtomically) {
    WaitForGraph graph;
    for (TransactionId txn_id : {1, 2, 3}) graph.add_node(txn_id);

    const std::vector<TransactionId> blockers = {2, 3, 2};
    EXPECT_TRUE(graph.add_edges(1, blockers));

    // Both dependencies were installed, and the duplicate was a no-op.
    EXPECT_FALSE(graph.add_edge(2, 1));
    EXPECT_FALSE(graph.add_edge(3, 1));
}

TEST(WaitForGraphTest, RejectsCompleteEdgeSetWithoutPartialInsertion) {
    WaitForGraph graph;
    for (TransactionId txn_id : {1, 2, 3}) graph.add_node(txn_id);
    EXPECT_TRUE(graph.add_edge(2, 3));

    const std::vector<TransactionId> blockers = {1, 2};
    EXPECT_FALSE(graph.add_edges(3, blockers));

    // The safe 3 -> 1 edge preceding the cyclic 3 -> 2 edge was not retained.
    EXPECT_TRUE(graph.add_edge(1, 3));
}

TEST(WaitForGraphTest, RejectsMissingBatchDestinationWithoutPartialInsertion) {
    WaitForGraph graph;
    graph.add_node(1);
    graph.add_node(2);

    const std::vector<TransactionId> blockers = {2, 99};
    EXPECT_FALSE(graph.add_edges(1, blockers));

    // The valid 1 -> 2 edge preceding the missing node was not retained.
    EXPECT_TRUE(graph.add_edge(2, 1));
}

TEST(WaitForGraphTest, RemovingNodeRemovesIncomingAndOutgoingEdges) {
    WaitForGraph graph;
    for (TransactionId txn_id : {1, 2, 3}) graph.add_node(txn_id);
    EXPECT_TRUE(graph.add_edge(1, 2));
    EXPECT_TRUE(graph.add_edge(2, 3));

    graph.remove(2);
    graph.add_node(2);

    // Neither old edge remains after the node is recreated.
    EXPECT_TRUE(graph.add_edge(2, 1));
    EXPECT_TRUE(graph.add_edge(3, 2));
}

TEST(WaitForGraphTest, RemovingOutgoingEdgesPreservesIncomingEdges) {
    WaitForGraph graph;
    for (TransactionId txn_id : {1, 2, 3}) graph.add_node(txn_id);
    EXPECT_TRUE(graph.add_edge(1, 2));
    EXPECT_TRUE(graph.add_edge(3, 1));

    graph.remove_outgoing(1);

    // The old 1 -> 2 dependency is gone.
    EXPECT_TRUE(graph.add_edge(2, 1));

    // The existing 3 -> 1 dependency remains, so 1 -> 3 is still a cycle.
    EXPECT_FALSE(graph.add_edge(1, 3));
}

TEST(WaitForGraphTest, ConcurrentOperationsKeepGraphConsistent) {
    WaitForGraph graph;
    constexpr TransactionId node_count = 64;
    for (TransactionId txn_id = 1; txn_id <= node_count; ++txn_id) graph.add_node(txn_id);

    std::vector<std::thread> threads;
    for (TransactionId start = 1; start <= 8; ++start) {
        threads.emplace_back([&, start] {
            for (TransactionId txn_id = start; txn_id < node_count; txn_id += 8) {
                EXPECT_TRUE(graph.add_edge(txn_id, txn_id + 1));
                graph.remove_edge(txn_id, txn_id + 1);
            }
        });
    }
    for (auto& thread : threads) thread.join();

    for (TransactionId txn_id = 1; txn_id <= node_count; ++txn_id) {
        EXPECT_TRUE(graph.exists(txn_id));
    }
}

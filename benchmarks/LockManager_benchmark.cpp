#include <KeyCodec.h>
#include <LockManager/LockManager.h>
#include <Log/Index.h>
#include <Log/Log.h>
#include <TransactionManager/TransactionManager.h>
#include <TransactionManager/WaitForGraph.h>

#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

class TempDir {
public:
    TempDir() {
        std::random_device random;
        for (std::size_t attempt = 0; attempt < 100; ++attempt) {
            path_ = std::filesystem::temp_directory_path() /
                ("stoneleaf-lock-benchmark-" + std::to_string(random()));
            if (std::filesystem::create_directory(path_)) return;
        }
        throw std::runtime_error("Failed to create benchmark directory");
    }

    ~TempDir() { std::filesystem::remove_all(path_); }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

class NoopUndoExecutor final : public TransactionUndoExecutor {
public:
    std::vector<PageEffect> undo(
        Transaction&,
        const UndoDescriptor&) override {
        return {};
    }
};

Config benchmark_log_config() {
    return {
        .max_index_bytes = Index::ENTRY_SIZE * 1'000'000,
        .max_store_bytes = 64 * 1024 * 1024,
        .initial_lsn = 1,
    };
}

template<typename Function>
void measure(const std::string& name, std::uint64_t operations, Function function) {
    const auto start = Clock::now();
    function();
    const auto elapsed = Clock::now() - start;
    const double seconds = std::chrono::duration<double>(elapsed).count();
    const double nanoseconds_per_operation = seconds * 1'000'000'000.0 /
        static_cast<double>(operations);
    const double operations_per_second = static_cast<double>(operations) / seconds;

    std::cout << std::left << std::setw(47) << name
              << std::right << std::setw(12) << std::fixed << std::setprecision(1)
              << nanoseconds_per_operation << " ns/op  "
              << std::setw(12) << std::setprecision(0)
              << operations_per_second << " ops/s\n";
}

void require_status(LockManagerStatus actual, LockManagerStatus expected) {
    if (actual != expected) {
        throw std::runtime_error("Unexpected lock-manager status");
    }
}

void benchmark_uncontended_locks(std::uint64_t iterations) {
    LockManager manager;
    const Key key = KeyCodec::make_string("uncontended");

    measure("LockManager shared acquire + release", iterations, [&] {
        for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
            require_status(manager.lock_shared(1, key), LockManagerStatus::Success);
            require_status(manager.unlock_shared(1, key), LockManagerStatus::Success);
        }
    });

    measure("LockManager exclusive acquire + release", iterations, [&] {
        for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
            require_status(manager.lock_exclusive(1, key), LockManagerStatus::Success);
            require_status(manager.unlock_exclusive(1, key), LockManagerStatus::Success);
        }
    });
}

void benchmark_parallel_locks(std::uint64_t iterations, std::size_t thread_count) {
    LockManager manager;
    std::vector<Key> keys;
    keys.reserve(thread_count);
    for (std::size_t thread = 0; thread < thread_count; ++thread) {
        keys.push_back(KeyCodec::make_string("thread-" + std::to_string(thread)));
    }

    const std::uint64_t operations = iterations * thread_count;
    measure("LockManager parallel distinct-key X round trip", operations, [&] {
        std::barrier start_line(static_cast<std::ptrdiff_t>(thread_count));
        std::vector<std::thread> threads;
        threads.reserve(thread_count);

        for (std::size_t thread = 0; thread < thread_count; ++thread) {
            threads.emplace_back([&, thread] {
                start_line.arrive_and_wait();
                for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
                    const TransactionId txn_id = thread + 1;
                    require_status(
                        manager.lock_exclusive(txn_id, keys[thread]),
                        LockManagerStatus::Success);
                    require_status(
                        manager.unlock_exclusive(txn_id, keys[thread]),
                        LockManagerStatus::Success);
                }
            });
        }
        for (std::thread& thread : threads) thread.join();
    });

    const Key hot_key = KeyCodec::make_string("hot-key");
    measure("LockManager parallel shared hot-key round trip", operations, [&] {
        std::barrier start_line(static_cast<std::ptrdiff_t>(thread_count));
        std::vector<std::thread> threads;
        threads.reserve(thread_count);

        for (std::size_t thread = 0; thread < thread_count; ++thread) {
            threads.emplace_back([&, thread] {
                start_line.arrive_and_wait();
                for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
                    const TransactionId txn_id = thread + 1;
                    require_status(
                        manager.lock_shared(txn_id, hot_key),
                        LockManagerStatus::Success);
                    require_status(
                        manager.unlock_shared(txn_id, hot_key),
                        LockManagerStatus::Success);
                }
            });
        }
        for (std::thread& thread : threads) thread.join();
    });
}

void benchmark_wait_for_graph(std::uint64_t iterations) {
    constexpr TransactionId node_count = 1024;
    WaitForGraph graph;
    for (TransactionId txn_id = 1; txn_id <= node_count; ++txn_id) {
        graph.add_node(txn_id);
    }

    measure("WaitForGraph add/remove batch of 4 edges", iterations, [&] {
        for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
            const TransactionId source = iteration % node_count + 1;
            const std::array<TransactionId, 4> blockers{
                source % node_count + 1,
                (source + 1) % node_count + 1,
                (source + 2) % node_count + 1,
                (source + 3) % node_count + 1,
            };
            if (!graph.add_edges(source, blockers)) {
                throw std::runtime_error("Acyclic edge batch was rejected");
            }
            graph.remove_outgoing(source);
        }
    });

    WaitForGraph chain;
    for (TransactionId txn_id = 1; txn_id <= node_count; ++txn_id) {
        chain.add_node(txn_id);
    }
    for (TransactionId txn_id = 1; txn_id < node_count; ++txn_id) {
        if (!chain.add_edge(txn_id, txn_id + 1)) {
            throw std::runtime_error("Failed to build benchmark chain");
        }
    }

    const std::uint64_t cycle_checks = std::max<std::uint64_t>(1000, iterations / 20);
    measure("WaitForGraph detect cycle across 1024 nodes", cycle_checks, [&] {
        for (std::uint64_t iteration = 0; iteration < cycle_checks; ++iteration) {
            if (chain.add_edge(node_count, 1)) {
                throw std::runtime_error("Cycle was not detected");
            }
        }
    });
}

void benchmark_transaction_integration(
    std::uint64_t iterations,
    std::size_t thread_count) {
    TempDir directory;
    Log log(benchmark_log_config());
    log.open(directory.path().string());
    LockManager lock_manager;
    NoopUndoExecutor undo_executor;
    TransactionManager transaction_manager(log, lock_manager, undo_executor);

    constexpr std::size_t transaction_count = 128;
    std::vector<TransactionHandle> transactions;
    transactions.reserve(transaction_count);
    for (std::size_t index = 0; index < transaction_count; ++index) {
        transactions.push_back(transaction_manager.begin());
    }

    measure("TransactionManager register/remove 4 blockers", iterations, [&] {
        for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
            const std::size_t source_index = iteration % transaction_count;
            const std::array<TransactionId, 4> blockers{
                transactions[(source_index + 1) % transaction_count]->id(),
                transactions[(source_index + 2) % transaction_count]->id(),
                transactions[(source_index + 3) % transaction_count]->id(),
                transactions[(source_index + 4) % transaction_count]->id(),
            };
            const TransactionId source = transactions[source_index]->id();
            if (transaction_manager.register_wait(source, blockers) !=
                WaitRegistrationStatus::Registered) {
                throw std::runtime_error("Transaction wait registration failed");
            }
            transaction_manager.remove_wait(source);
        }
    });

    const std::uint64_t lifecycle_iterations = std::min<std::uint64_t>(iterations, 500);
    const Key key = KeyCodec::make_string("transaction-lock");
    measure("Transaction begin + X lock + durable commit", lifecycle_iterations, [&] {
        for (std::uint64_t iteration = 0; iteration < lifecycle_iterations; ++iteration) {
            TransactionHandle transaction = transaction_manager.begin();
            require_status(
                lock_manager.lock_exclusive(transaction->id(), key),
                LockManagerStatus::Success);
            if (transaction_manager.commit(transaction) != CommitStatus::Success) {
                throw std::runtime_error("Transaction commit failed");
            }
        }
    });

    std::vector<Key> client_keys;
    client_keys.reserve(thread_count);
    for (std::size_t thread = 0; thread < thread_count; ++thread) {
        client_keys.push_back(
            KeyCodec::make_string("transaction-client-" + std::to_string(thread)));
    }

    const std::uint64_t parallel_transactions = lifecycle_iterations * thread_count;
    measure("Parallel begin + X lock + durable commit", parallel_transactions, [&] {
        std::barrier start_line(static_cast<std::ptrdiff_t>(thread_count));
        std::vector<std::thread> clients;
        clients.reserve(thread_count);

        for (std::size_t thread = 0; thread < thread_count; ++thread) {
            clients.emplace_back([&, thread] {
                start_line.arrive_and_wait();
                for (std::uint64_t iteration = 0;
                     iteration < lifecycle_iterations;
                     ++iteration) {
                    TransactionHandle transaction = transaction_manager.begin();
                    require_status(
                        lock_manager.lock_exclusive(
                            transaction->id(),
                            client_keys[thread]),
                        LockManagerStatus::Success);
                    if (transaction_manager.commit(transaction) !=
                        CommitStatus::Success) {
                        throw std::runtime_error("Parallel transaction commit failed");
                    }
                }
            });
        }
        for (std::thread& client : clients) client.join();
    });
}

} // namespace

int main(int argc, char** argv) {
    const std::uint64_t iterations = argc > 1
        ? std::stoull(argv[1])
        : 100'000;
    const std::size_t detected_threads = std::max(1u, std::thread::hardware_concurrency());
    const std::size_t thread_count = argc > 2
        ? std::stoull(argv[2])
        : std::min<std::size_t>(detected_threads, 8);

    if (iterations == 0 || thread_count == 0) {
        std::cerr << "Iterations and thread count must be nonzero\n";
        return 1;
    }

    try {
        std::cout << "StoneleafDB lock benchmark\n"
                  << "iterations/thread: " << iterations << '\n'
                  << "threads: " << thread_count << '\n'
                  << "parallel ns/op is reciprocal aggregate throughput, not client latency\n\n";

        benchmark_uncontended_locks(iterations);
        benchmark_parallel_locks(iterations, thread_count);
        benchmark_wait_for_graph(iterations);
        benchmark_transaction_integration(iterations, thread_count);
    } catch (const std::exception& error) {
        std::cerr << "Benchmark failed: " << error.what() << '\n';
        return 1;
    }
}

#include <KeyStore.h>
#include <LockManager/LockManager.h>
#include <Log/Log.h>
#include <Log/WalPayloadCodec.h>
#include <Log/WalRecords.h>
#include <TransactionManager/TransactionManager.h>
#include <server/CommandServer.h>
#include <DiskIO.h>
#include <queue>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <span>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <filesystem>
#include <Log/WalRecordCodec.h>
#include <unordered_map>

namespace {

constexpr std::uint16_t SERVER_PORT = 8080;
enum class StartupStatus : std::uint8_t {
    SUCCESS = 0,
    FAILED,
};

// Returns {base_lsn, is_store}. Mirrors Log.cpp's own parse_segment_name:
// a name starting with "segment-" must have the exact "<20 digits>.store" or
// "<20 digits>.index" shape, or it's treated as corruption, not ignored.
std::pair<std::uint64_t, bool> parse_segment_offset(const std::string& name) {
    constexpr std::size_t prefix_size = 8;
    constexpr std::size_t digits_size = 20;
    if (!name.starts_with("segment-")) return {0, false};
    const bool store = name.size() == prefix_size + digits_size + 6 && name.ends_with(".store");
    const bool index = name.size() == prefix_size + digits_size + 6 && name.ends_with(".index");
    if (!store && !index) throw std::runtime_error("Malformed WAL segment filename");
    const auto digits = name.substr(prefix_size, digits_size);
    if (digits.find_first_not_of("0123456789") != std::string::npos) throw std::runtime_error("Malformed WAL segment filename");
    try { return {std::stoull(digits), store}; }
    catch (...) { throw std::runtime_error("Malformed WAL segment filename"); }
}

Config setup_config(const std::string& database_directory) {
    // Log::open() requires initial_lsn to equal the smallest existing
    // segment's base LSN when segments already exist. A fresh database (no
    // WAL directory yet, or an empty one) has no existing segment to match,
    // so it starts a brand new log at LSN 1.
    const std::string wal_directory = database_directory + ".wal";
    std::optional<std::uint64_t> smallest_base;

    if (std::filesystem::exists(wal_directory)) {
        for (const auto& entry : std::filesystem::directory_iterator(wal_directory)) {
            const auto name = entry.path().filename().string();
            const auto [base, is_store] = parse_segment_offset(name);
            // Every segment has exactly one .store file, so counting only
            // those naturally avoids double-counting its paired .index file.
            if (!is_store) continue;
            if (!smallest_base || base < *smallest_base) smallest_base = base;
        }
    }

    return Config{
        .max_index_bytes = 1024 * Index::ENTRY_SIZE,
        .max_store_bytes = 64 * 1024 * 1024,
        .initial_lsn = smallest_base.value_or(1),
    };
}

void redo_page_effects(int db_fd, Log& log, std::vector<PageEffect>& page_effects) {
    // db_fd is opened once by the caller for the whole redo pass and synced
    // once at the end, rather than per record.
    for (const PageEffect &effect : page_effects) {
        switch (effect.kind) {
            case PageEffectKind::Write:
            case PageEffectKind::Allocate:
            case PageEffectKind::Free:
                // Every kind carries a complete after-image (see
                // page_effect() in Pager.cpp), so redo is the same physical
                // write regardless of which structural event produced it.
                // PageEffect already carries page_num directly, so no PageV2
                // decode is needed just to find where this page lives.
                disk::write_exact_at(
                    db_fd,
                    std::span<const char>(effect.after_image),
                    static_cast<std::streamoff>(effect.page_num) * static_cast<std::streamoff>(PAGE_SIZE));
        }
    }

}
// Analysis + Redo. Runs against the raw database file directly - no live
// Pager/BTree/KeyStore is open yet, since redo is pure physical byte replay
// and doesn't need B-tree logic. Populates unresolved_transactions with every
// transaction that has no TxnCommit/TxnEnd by the end of the retained log, so
// aries_recovery_undo (called after the caller opens KeyStore for real) knows
// what still needs undoing.
void aries_recovery_redo(
    Log& log,
    const std::string& db_file_name,
    std::unordered_map<TransactionId, Lsn>& unresolved_transactions
) {
    // A genuinely fresh database has no database file yet - KeyStore::open
    // (called by the caller right after this) is what creates it. There is
    // nothing to redo against a file that was never created.
    if (!std::filesystem::exists(db_file_name)) return;

    Lsn current_offset = log.base_segment_offest();

    int db_fd = disk::open_file(db_file_name, O_RDWR);
    while (true) {
        try {
            WalRecord record = log.read(current_offset);
            WalRecordType record_type = record.type;
            TransactionId txn_id = static_cast<TransactionId>(record.transaction_id);

            switch (record_type) {
                case WalRecordType::TxnBegin: {
                    unresolved_transactions[static_cast<TransactionId>(record.transaction_id)] = current_offset;
                    current_offset += 1;
                    continue;
                }
                case WalRecordType::BTreeAction: {
                    WalPayload payload = WalPayloadCodec::decode(record_type, record.data);
                    redo_page_effects(db_fd, log, std::get<BTreeActionPayload>(payload).effects);
                    current_offset += 1;
                    unresolved_transactions[txn_id] = static_cast<Lsn>(record.lsn);
                    continue;
                }
                case WalRecordType::Compensation: {
                    WalPayload payload = WalPayloadCodec::decode(record_type, record.data);
                    redo_page_effects(db_fd, log, std::get<CompensationPayload>(payload).effects);
                    current_offset += 1;
                    unresolved_transactions[txn_id] = static_cast<Lsn>(record.lsn);
                    continue;
                }
                case WalRecordType::SystemAction: {
                    WalPayload payload = WalPayloadCodec::decode(record_type, record.data);
                    redo_page_effects(db_fd, log, std::get<SystemActionPayload>(payload).effects);
                    current_offset += 1;
                    continue;
                }
                case WalRecordType::TxnCommit: {
                    auto it = unresolved_transactions.find(txn_id);
                    if (it != unresolved_transactions.end()) {
                        unresolved_transactions.erase(txn_id);
                    }
                    current_offset += 1;
                    continue;
                }
                case WalRecordType::TxnEnd: {
                    auto it = unresolved_transactions.find(txn_id);
                    if (it != unresolved_transactions.end()) {
                        unresolved_transactions.erase(txn_id);
                    }
                    current_offset += 1;
                    continue;
                }
                case WalRecordType::TxnAbort: {
                    // The abort decision alone carries no PageEffects and
                    // must not move where undo resumes for this transaction:
                    // live abort() captures undo_lsn = last_lsn_ before
                    // appending this record, so its own undo walk never
                    // visits the TxnAbort record itself. Leave whatever LSN
                    // is already tracked (the transaction's last BTreeAction/
                    // Compensation, or its TxnBegin if it never wrote one)
                    // untouched - just keep it in the unresolved set.
                    current_offset += 1;
                    continue;
                }

            }
        } catch (const std::out_of_range&) {
            // Log::read throws this specifically when current_offset has
            // reached the end of the retained log - that's the normal,
            // expected way this loop terminates. Any other exception type
            // (e.g. WalPayloadCodec::decode's "Malformed..." runtime_errors)
            // means a genuinely corrupt record, and is deliberately left to
            // propagate out of aries_recovery_redo uncaught rather than being
            // silently swallowed here.
            break;
        }
    }

    // Redo has applied every logged page effect for this pass; make it
    // durable once for the whole pass rather than after each record.
    disk::sync_file_to_disk_fd(db_fd);
    disk::close_file(db_fd);
}

// Undo, for whatever aries_recovery_redo left in unresolved_transactions.
// Runs through the live KeyStore/BTree (not a raw fd) - unlike redo, undo has
// to navigate and mutate real B-tree structure, so the caller must open
// KeyStore before calling this. NOT YET FINISHED: the BTreeAction case below
// still needs to call key_store.undo(...) to actually run the logical inverse
// and append a Compensation record.
void aries_recovery_undo(
    Log& log,
    KeyStore& key_store,
    std::unordered_map<TransactionId, Lsn>& unresolved_transactions
) {
    // Redo pass is done. Now we need to do the unlogical undo passes for the remanining unresolved
    // transactions. We need to process them in descending order of their LSN. Need to build a priority
    // queue for this. Each item in the priority queue will be a pair where the first element is the 
    // LSN and the second element is the transaction id.

    // Let's define the functor to define the ordering of elements in the max-heap
    struct CompareLsn {
        bool operator()(const std::pair<Lsn, TransactionId>& a, const std::pair<Lsn, TransactionId>& b) const {
            return a.first < b.first; // Yields Max-Heap (Largest Lsn on top)
        }
    };

    std::priority_queue<
        std::pair<Lsn, TransactionId>, 
        std::vector<std::pair<Lsn, TransactionId>>, 
        CompareLsn
    > lsns_max_heap;

    // Build the max heap
    for (const auto& [transaction_id, last_lsn] : unresolved_transactions) {
        lsns_max_heap.push({last_lsn, transaction_id});
    }

    // [Clear the map] NO
   //  unresolved_transactions.clear();
   // Don't clear the unresolved transactions map. we will need the last lsn on 
    // each transaction id when we add new records so we can update it and when we
    // append the txn end record, we need the prev_lsn which will be the last lsn

    // Now, let's iterate over the LSNs and undo the logical records
    while (!lsns_max_heap.empty()) {
        std::pair<Lsn, TransactionId> target = lsns_max_heap.top();
        lsns_max_heap.pop();

        WalRecord record = log.read(target.first);
        WalRecordType record_type = record.type;

        switch (record_type) {
            case WalRecordType::TxnBegin: {
                // Need to append a TxnEnd record
                PendingWalRecord pending_end_record{
                    .type = WalRecordType::TxnEnd,
                    .transaction_id = static_cast<std::uint64_t>(target.second),
                    .prev_lsn = unresolved_transactions[target.second],
                    .data = std::vector<char>{},
                };

                // Append the record and remove the transaction from the map
                log.append(pending_end_record);
                unresolved_transactions.erase(target.second);
                continue;
            }

            case WalRecordType::BTreeAction: {
                // Now, what the hell do we even do here.
                // Let's decode the payload first

                BTreeActionPayload payload = std::get<BTreeActionPayload>(WalPayloadCodec::decode(record_type, record.data));
                
                Transaction transaction(target.second);
                transaction.set_last_lsn(unresolved_transactions[target.second]);

                Lsn new_lsn = 0;
                auto append_compensation = [&](std::vector<PageEffect> effects) -> Lsn {
                    const CompensationPayload compensation{
                        .undo_of_lsn = record.lsn,
                        .undo_next_lsn = record.prev_lsn,
                        .effects = std::move(effects),
                    };
                    new_lsn = log.append(WalRecords::compensation(target.second, transaction.last_lsn(), compensation));
                    transaction.set_last_lsn(new_lsn);
                    return new_lsn;
                };

                key_store.undo(transaction, payload.undo, append_compensation);

                // Now, reset the last lsn on this transaction and also push the next entry onto the heap
                unresolved_transactions[target.second] = new_lsn;
                lsns_max_heap.push({record.prev_lsn, target.second});
                continue;
            }
        case WalRecordType::Compensation: {
            CompensationPayload payload = std::get<CompensationPayload>(WalPayloadCodec::decode(record_type, record.data));

            // Nothing to do in the undo phase. It was already undone by an earlier recovery process
            // Keep the last lsn in unresolved transaction as it is since we didnt append another
            // record associated with this txn
            lsns_max_heap.push({payload.undo_next_lsn, target.second});
            continue;
        }
        // We will ignore system action
        // We will also not handle commit. cuz techincally, this should be here
        case WalRecordType::TxnAbort: {
            // Nothing to do, just push the next prev lsn
            lsns_max_heap.push({record.prev_lsn, target.second});
            continue;
        }

        // We also dont need a case for txn end. this practically wouldnt be here
        }

    }

    // Finally, sync the log file
    if (log.next_lsn() > log.base_segment_offest()) {
        log.sync_through(log.next_lsn() - 1);
    }

    // Now we need to flush the dirtied pages by the wal undo pass
    KeyStoreStatus flush_status = key_store.flush();
    if (flush_status != KeyStoreStatus::Success) {
        // A hard abort() here gives no message and no chance for the caller
        // to report a clean startup failure. Throw instead, consistent with
        // every other recovery failure path, and let setup_database's catch
        // turn it into a reported StartupStatus::FAILED.
        throw std::runtime_error("Failed to flush recovery-dirtied pages to the database file");
    }
}

// Deletes every WAL segment that isn't the currently active one. Safe only
// once redo has replayed the entire retained log onto the database file and
// flush() has made every page undo dirtied durable there too - at that
// point nothing outside the active segment is needed to recover again, even
// if the active segment rolled over partway through the undo pass.
void cleanup_finalized_segments(const std::string& db_file_name) {
    const std::string wal_directory = db_file_name + ".wal";
    if (!std::filesystem::exists(wal_directory)) return;

    // The active segment is the one with the largest base LSN - segments are
    // created with strictly increasing base LSNs, and Log always appends
    // into the most recently created one.
    std::optional<std::uint64_t> active_base;
    for (const auto& entry : std::filesystem::directory_iterator(wal_directory)) {
        const auto name = entry.path().filename().string();
        if (!name.starts_with("segment-")) continue;
        const auto [base, is_store] = parse_segment_offset(name);
        if (!is_store) continue;
        if (!active_base || base > *active_base) active_base = base;
    }
    // No segments at all means nothing to clean up.
    if (!active_base) return;

    // Delete both the .store and .index file for every non-active base LSN.
    for (const auto& entry : std::filesystem::directory_iterator(wal_directory)) {
        const auto name = entry.path().filename().string();
        if (!name.starts_with("segment-")) continue;
        const auto [base, is_store] = parse_segment_offset(name);
        (void)is_store;
        if (base == *active_base) continue;
        std::filesystem::remove(entry.path());
    }
}

// KeyStore, Log, and TransactionManager all have deleted copy/move
// constructors, so they can't be built here and handed back by value - the
// caller (main) constructs them in the scope that needs to outlive this
// call (the connection-accept loop), and this function only configures and
// opens them.
StartupStatus setup_database(
    const std::string &db_file,
    KeyStore &key_store,
    Log &log,
    TransactionManager &transaction_manager
) {
    key_store.attach_transaction_manager(transaction_manager);

    try {
        log.open(db_file + ".wal");
    } catch (const std::exception &error) {
        std::cerr << "[ERROR] Failed to open WAL: " << error.what() << std::endl;
        return StartupStatus::FAILED;
    }

    try {
        // Redo runs against the raw database file, before KeyStore/BTree/Pager
        // are ever opened - opening them first would let Pager cache header
        // state that redo's direct writes would then leave stale underneath it.
        std::unordered_map<TransactionId, Lsn> unresolved_transactions;
        aries_recovery_redo(log, db_file, unresolved_transactions);

        if (key_store.open(db_file) != KeyStoreStatus::Success) {
            std::cerr << "[ERROR] Failed to open database: " << db_file << std::endl;
            return StartupStatus::FAILED;
        }

        // Undo needs the live KeyStore/BTree to actually navigate and mutate
        // the tree.
        aries_recovery_undo(log, key_store, unresolved_transactions);
        cleanup_finalized_segments(db_file);
    } catch (const std::exception &error) {
        std::cerr << "[ERROR] Recovery failed: " << error.what() << std::endl;
        return StartupStatus::FAILED;
    }

    return StartupStatus::SUCCESS;
}

int create_listener() {
    // Keep socket setup in one place so every startup failure closes the
    // partially initialized descriptor before returning to main.
    const int listener_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener_fd < 0) return -1;

    const int reuse_address = 1;
    if (::setsockopt(listener_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address)) != 0) {
        ::close(listener_fd);
        return -1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(SERVER_PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(listener_fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
        ::close(listener_fd);
        return -1;
    }

    if (::listen(listener_fd, SOMAXCONN) != 0) {
        ::close(listener_fd);
        return -1;
    }

    return listener_fd;
}

} // namespace

int main(int argc, char *argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <db-file>" << std::endl;
        return 1;
    }
    const std::string db_file = argv[1];

    KeyStore key_store;
    Config config = setup_config(db_file);
    Log log(config);
    LockManager lock_manager;
    TransactionManager transaction_manager(log, lock_manager, key_store);

    StartupStatus status = setup_database(db_file, key_store, log, transaction_manager);
    if (status == StartupStatus::FAILED) {
        return 1;
    }
    const int listener_fd = create_listener();
    if (listener_fd < 0) {
        std::cerr << "[ERROR] Failed to listen on 127.0.0.1:" << SERVER_PORT << std::endl;
        return 1;
    }

    std::cout << "Listening on 127.0.0.1:" << SERVER_PORT << std::endl;

    while (true) {
        sockaddr_in client_address{};
        socklen_t client_address_size = sizeof(client_address);
        const int socket_fd = ::accept(listener_fd, reinterpret_cast<sockaddr *>(&client_address), &client_address_size);
        if (socket_fd < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[ERROR] Failed to accept client connection" << std::endl;
            continue;
        }

        // The dispatcher owns the accepted descriptor. Its executor threads
        // remain joined, while this connection-level thread runs independently.
        try {
            std::thread dispatcher(
                CommandServer::serve_connection,
                socket_fd,
                std::ref(key_store),
                std::ref(transaction_manager));
            dispatcher.detach();
        } catch (...) {
            ::close(socket_fd);
        }
    }
}

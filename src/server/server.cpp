#include <KeyStore.h>
#include <LockManager/LockManager.h>
#include <Log/Log.h>
#include <Log/WalPayloadCodec.h>
#include <TransactionManager/TransactionManager.h>
#include <server/CommandServer.h>
#include <DiskIO.h>

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

enum class AriesStatus : std::uint8_t {
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
AriesStatus aries_recovery(KeyStore& key_store, TransactionManager& _transaction_manager, Log& log, std::string db_file_name) {
    std::unordered_map<TransactionId, Lsn> unresolved_transactions;
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
            // propagate out of aries_recovery uncaught rather than being
            // silently swallowed here.
            break;
        }
    }

    // Redo has applied every logged page effect for this pass; make it
    // durable once for the whole pass rather than after each record.
    disk::sync_file_to_disk_fd(db_fd);
    disk::close_file(db_fd);
}
// TODO: Need to return the keystore and everything to main
StartupStatus setup_database(int argc, char *argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <db-file>" << std::endl;
        return StartupStatus::FAILED;
    }

    KeyStore key_store;

    // First thing, let's set up the config appropriately:
    Config config = setup_config(argv[1]);

    Log log(config);
    LockManager lock_manager;
    TransactionManager transaction_manager(log, lock_manager, key_store);
    key_store.attach_transaction_manager(transaction_manager);

    try {
        log.open(std::string{argv[1]} + ".wal");
    } catch (const std::exception &error) {
        std::cerr << "[ERROR] Failed to open WAL: " << error.what() << std::endl;
        return StartupStatus::FAILED;
    }
    if (key_store.open(std::string{argv[1]}) != KeyStoreStatus::Success) {
        std::cerr << "[ERROR] Failed to open database: " << argv[1] << std::endl;
        return StartupStatus::FAILED;
    }

    // Now, we need to do ARIES recovery

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

    StartupStatus status = setup_database(argc, argv);
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

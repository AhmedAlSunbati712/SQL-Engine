#pragma once

#include <KeyStore.h>
#include <TransactionManager/TransactionManager.h>

struct SessionContext {
    TransactionHandle active_transaction;
};

namespace CommandServer {

void serve_connection(
    int socket_fd,
    KeyStore &key_store,
    TransactionManager &transaction_manager) noexcept;

} // namespace CommandServer

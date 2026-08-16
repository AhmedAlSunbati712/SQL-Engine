#pragma once

#include <KeyStore.h>
#include <TransactionManager/TransactionManager.h>

struct SessionContext {
    TransactionHandle active_transaction;
};

namespace CommandServer {

void handle_get(KeyStore &key_store, KeyStoreGetResult &result, const Key &key);
void handle_put(KeyStore &key_store, KeyStoreStatus &result, const Key &key, const Value &value);
void handle_remove(KeyStore &key_store, KeyStoreRemoveResult &result, const Key &key);
void handle_begin_transaction(KeyStore &key_store, KeyStoreStatus &result);
void handle_commit(KeyStore &key_store, KeyStoreStatus &result);
void handle_rollback(KeyStore &key_store, KeyStoreStatus &result);
void serve_connection(int socket_fd, KeyStore &key_store) noexcept;
void serve_transactional_connection(
    int socket_fd,
    KeyStore &key_store,
    TransactionManager &transaction_manager) noexcept;

} // namespace CommandServer

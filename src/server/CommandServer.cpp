#include <server/CommandServer.h>

namespace CommandServer {

void handle_get(KeyStore &key_store, KeyStoreGetResult &result, const Key &key) {
    result = key_store.get(key);
}

void handle_put(KeyStore &key_store, KeyStoreStatus &result, const Key &key, const Value &value) {
    result = key_store.put(key, value);
}

void handle_remove(KeyStore &key_store, KeyStoreRemoveResult &result, const Key &key) {
    result = key_store.remove(key);
}

void handle_begin_transaction(KeyStore &key_store, KeyStoreStatus &result) {
    result = key_store.begin_write_transaction();
}

void handle_commit(KeyStore &key_store, KeyStoreStatus &result) {
    result = key_store.commit();
}

void handle_rollback(KeyStore &key_store, KeyStoreStatus &result) {
    result = key_store.rollback();
}

} // namespace CommandServer

#include <Client.h>

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>

namespace {

Client create_client(const std::string &ip_address, int port) {
    try {
        return Client(ip_address, port);
    } catch (const std::exception &error) {
        std::cerr << "[ERROR] Failed to create client: " << error.what() << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

Session &create_session(Client &client) {
    try {
        return client.new_session();
    } catch (const std::exception &error) {
        std::cerr << "[ERROR] Failed to connect session: " << error.what() << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

void expect_value(Session &session, const KeyInput &key, const ValueInput &expected) {
    const std::optional<ValueInput> value = session.get(key);
    if (!value.has_value()) {
        throw std::runtime_error("[ERROR] Expected key to exist");
    }

    if (*value != expected) {
        throw std::runtime_error("[ERROR] Key contained an unexpected value");
    }

    std::cout << "========== GET returned ";
    std::visit([](const auto &decoded_value) { std::cout << decoded_value; }, *value);
    std::cout << " ==========" << std::endl;
}

void expect_missing(Session &session, const KeyInput &key) {
    if (session.get(key).has_value()) {
        throw std::runtime_error("[ERROR] Expected key to be missing");
    }

    std::cout << "========== GET confirmed the key is missing ==========" << std::endl;
}

} // namespace

int main() {
    Client client = create_client("127.0.0.1", 8080);
    std::cout << "========== Successfully created a client ==========" << std::endl;

    Session &session = create_session(client);
    const int session_id = session.get_id();
    std::cout << "========== Successfully connected with session id " << session_id << " ==========" << std::endl;

    int exit_code = 0;
    try {
        const KeyInput key = std::string{"myown_key"};
        const ValueInput initial_value = std::uint64_t{1};
        session.put(key, initial_value);
        std::cout << "========== Successfully set 'myown_key' to 1 ==========" << std::endl;
        expect_value(session, key, initial_value);

        session.begin_transaction();
        session.put(key, ValueInput{std::uint64_t{2}});
        session.rollback();
        std::cout << "========== Successfully rolled back value 2 ==========" << std::endl;
        expect_value(session, key, initial_value);

        const ValueInput committed_value = std::uint64_t{3};
        session.begin_transaction();
        session.put(key, committed_value);
        session.commit();
        std::cout << "========== Successfully committed value 3 ==========" << std::endl;
        expect_value(session, key, committed_value);

        session.remove(key);
        std::cout << "========== Successfully removed 'myown_key' ==========" << std::endl;
        expect_missing(session, key);
    } catch (const std::exception &error) {
        std::cerr << error.what() << std::endl;
        exit_code = 1;
    }

    client.close_session(session_id);
    std::cout << "========== Successfully closed the session ==========" << std::endl;
    return exit_code;
}

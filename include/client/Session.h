#pragma once
#include <Command.h>
#include <KeyCodec.h>
#include <ValueCodec.h>

class Session {
    public:
        Session(int fd_, int id_);
        ~Session();

        Session(const Session &) = delete;
        Session &operator=(const Session &) = delete;
        Session(Session &&other) noexcept;
        Session &operator=(Session &&other) noexcept;

        int get_id() const;
        std::optional<ValueInput> get(const KeyInput &key);
        void put(const KeyInput &key, const ValueInput &value);
        void remove(const KeyInput &key);
        void begin_transaction();
        void commit();
        void rollback();
        void close() noexcept;
    private:
        void send_command(const Command &command);
        void read_operation_response();
        std::optional<ValueInput> read_get_response();

        int fd;
        int id;

};

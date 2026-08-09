#pragma once
#include <KeyCodec.h>
#include <ValueCodec.h>

class Session {
    public:
        Session(int fd_, int id);
        ~Session();

        int get_id();
        std::optional<ValueInput> get(const KeyInput& key);
        void put(const KeyInput &key, const ValueInput &value);
        void remove(const KeyInput &key);
        void begin_transaction();
        void commit();
        void rollback();
        void close();
    private:
        int fd;
        int id;

};
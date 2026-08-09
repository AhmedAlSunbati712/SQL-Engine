#pragma once
#include <Session.h>
#include <string>
#include <netinet/in.h>
#include <unordered_map>

class Client {
    public:
        Client(std::string ip_addr, int port);
        ~Client();
        Session &new_session();
        void close_session(int session_id);
    private:
        struct sockaddr_in server_addr;
        std::unordered_map<int, std::unique_ptr<Session>> sessions;
        int seq_number; // Monotonically increasing number used to assign ids to sessions

};
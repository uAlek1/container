#pragma once

#include <cstdint>
#include <string>

#include "runtime.pb.h"

namespace cli {

class Client {
public:
    explicit Client(std::string socket_path);
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) noexcept;
    Client& operator=(Client&&) noexcept;

    void connect();

    void send(runtime::v1::ClientMessage& msg);

    bool recv(runtime::v1::ServerMessage& msg, bool check_request_id = true);

    uint64_t next_request_id() { return ++request_id_; }

    int fd() { return fd_; }

private:
    std::string socket_path_;
    int fd_ = -1;
    uint64_t request_id_ = 0;
    uint64_t last_sent_id_ = 0;
};

}  // namespace cli
#include "client.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "framing.hpp"

namespace cli {

Client::Client(std::string socket_path) : socket_path_(std::move(socket_path)) {}

Client::~Client() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

Client::Client(Client&& other) noexcept
    : socket_path_(std::move(other.socket_path_)),
      fd_(other.fd_),
      request_id_(other.request_id_),
      last_sent_id_(other.last_sent_id_) {
    other.fd_ = -1;
}

Client& Client::operator=(Client&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) ::close(fd_);
        socket_path_ = std::move(other.socket_path_);
        fd_ = other.fd_;
        request_id_ = other.request_id_;
        last_sent_id_ = other.last_sent_id_;
        other.fd_ = -1;
    }
    return *this;
}

void Client::connect() {
    if (fd_ >= 0) {
        throw std::runtime_error("Client::connect: already connected");
    }

    if (socket_path_.size() >= sizeof(sockaddr_un::sun_path)) {
        throw std::runtime_error(
            "socket path too long (max " +
            std::to_string(sizeof(sockaddr_un::sun_path) - 1) + " bytes): " +
            socket_path_);
    }

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error(std::string("socket(): ") + std::strerror(errno));
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, socket_path_.c_str(), socket_path_.size() + 1);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        int saved = errno;
        ::close(fd);
        throw std::runtime_error(
            "cannot connect to " + socket_path_ + ": " + std::strerror(saved) +
            " (is the daemon running?)");
    }

    fd_ = fd;
}

void Client::send(runtime::v1::ClientMessage& msg) {
    if (fd_ < 0) {
        throw std::runtime_error("Client::send: not connected");
    }

    if (msg.request_id() == 0) {
        msg.set_request_id(next_request_id());
    }
    last_sent_id_ = msg.request_id();

    std::string err;
    if (!common::write_message(fd_, msg, err)) {
        throw std::runtime_error("send: " + err);
    }
}

bool Client::recv(runtime::v1::ServerMessage& msg, bool check_request_id) {
    if (fd_ < 0) {
        throw std::runtime_error("Client::recv: not connected");
    }

    std::string err;
    if (!common::read_message(fd_, msg, err)) {
        if (err.empty()) {
            return false;  // чистый EOF
        }
        throw std::runtime_error("recv: " + err);
    }

    if (check_request_id && msg.request_id() != last_sent_id_) {
        throw std::runtime_error(
            "recv: request_id mismatch (expected " + std::to_string(last_sent_id_) +
            ", got " + std::to_string(msg.request_id()) + ")");
    }

    return true;
}

}  // namespace cli
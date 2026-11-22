#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "framing.hpp"
#include "path.hpp"
#include "registry.hpp"
#include "runtime_service.hpp"
#include "shim_manager.hpp"
#include "runtime.pb.h"

namespace {

std::atomic<bool> g_shutdown{false};

extern "C" void shutdown_handler(int) {
    g_shutdown.store(true, std::memory_order_relaxed);
}

int create_listen_socket(const std::string& path, std::string& err) {
    ::unlink(path.c_str());

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        err = std::string("socket(): ") + std::strerror(errno);
        return -1;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        err = std::string("bind(") + path + "): " + std::strerror(errno);
        ::close(fd);
        return -1;
    }

    if (::listen(fd, 64) < 0) {
        err = std::string("listen(): ") + std::strerror(errno);
        ::close(fd);
        return -1;
    }

    return fd;
}

void handle_client(int client_fd,
                   cont_daemon::RuntimeService& service) {

    auto send = [client_fd](const runtime::v1::ServerMessage& msg) {
        std::string err;
        if (!common::write_message(client_fd, msg, err)) {
            std::fprintf(stderr, "cont_daemon: write to client: %s\n", err.c_str());
            return false;
        }
        return true;
    };

    for (;;) {
        runtime::v1::ClientMessage req;
        std::string err;
        if (!common::read_message(client_fd, req, err)) {
            if (!err.empty()) {
                std::fprintf(stderr, "cont_daemon: read from client: %s\n", err.c_str());
            }
            break;
        }
        if (!service.handle(req, send)) {
            break;
        }
    }

    ::close(client_fd);
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);

    struct sigaction sa{};
    sa.sa_handler = shutdown_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // без SA_RESTART: accept должен возвращать EINTR
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    if (!std::getenv("XDG_RUNTIME_DIR")) {
        std::fprintf(stderr, "daemon: warning: XDG_RUNTIME_DIR not set, "
                             "using /tmp fallback\n");
    }

    if (!common::ensure_runtime_dirs()) {
        std::fprintf(stderr, "daemon: cannot create runtime directories\n");
        return 1;
    }

    cont_daemon::ContainerRegistry registry;
    auto shim = cont_daemon::make_stub_shim_manager();
    cont_daemon::RuntimeService service(registry, *shim);

    // Создать слушающий сокет.
    const std::string socket_path = common::daemon_socket_path().string();
    std::string err;
    int listen_fd = create_listen_socket(socket_path, err);
    if (listen_fd < 0) {
        std::fprintf(stderr, "daemon: %s\n", err.c_str());
        return 1;
    }

    std::fprintf(stderr, "daemon: listening on %s\n", socket_path.c_str());

    // Цикл accept.
    while (!g_shutdown.load(std::memory_order_relaxed)) {
        int client_fd = ::accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::fprintf(stderr, "cont_daemon: accept: %s\n", std::strerror(errno));
            break;
        }

        std::thread([client_fd, &service] {
            handle_client(client_fd, service);
        }).detach();
    }

    std::fprintf(stderr, "cont_daemon: shutting down\n");
    ::close(listen_fd);
    ::unlink(socket_path.c_str());
    return 0;
}
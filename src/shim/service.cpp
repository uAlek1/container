#include "service.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <signal.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>


#include "framing.hpp"
#include "container_process.hpp"

namespace shim {
namespace {

runtime::v1::Status ok_status() {
    runtime::v1::Status s;
    s.set_code(runtime::v1::STATUS_OK);
    return s;
}

runtime::v1::Status error_status(runtime::v1::StatusCode code, std::string msg) {
    runtime::v1::Status s;
    s.set_code(code);
    s.set_message(std::move(msg));
    return s;
}

int create_listen_socket(const std::string& path, std::string& err) {
    if (path.size() >= sizeof(sockaddr_un::sun_path)) {
        err = "socket path too long: " + path;
        return -1;
    }
    ::unlink(path.c_str());

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { err = std::string("socket: ") + std::strerror(errno); return -1; }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        err = std::string("bind: ") + std::strerror(errno);
        ::close(fd);
        return -1;
    }
    if (::listen(fd, 8) < 0) {
        err = std::string("listen: ") + std::strerror(errno);
        ::close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

ShimService::ShimService(std::string id, int32_t pid, int master_fd,
                         std::string socket_path)
    : id_(std::move(id)),
      pid_(pid),
      master_fd_(master_fd),
      socket_path_(std::move(socket_path)) {}

ShimService::~ShimService() {
    if (wait_thread_.joinable()) wait_thread_.join();
    if (master_fd_ >= 0) ::close(master_fd_);
}

void ShimService::wait_thread_fn() {
    int code = wait_for_exit(pid_);
    exit_code_.store(code, std::memory_order_relaxed);
    exited_.store(true, std::memory_order_release);
}

runtime::v1::Status ShimService::build_status(
    runtime::v1::ContainerState& out_state,
    int32_t& out_exit_code,
    int32_t& out_pid) {
    out_pid = pid_;
    if (exited_.load(std::memory_order_acquire)) {
        out_state = runtime::v1::CONTAINER_STATE_EXITED;
        out_exit_code = exit_code_.load(std::memory_order_relaxed);
    } else {
        out_state = runtime::v1::CONTAINER_STATE_RUNNING;
        out_exit_code = 0;
    }
    return ok_status();
}

void ShimService::handle_client(int client_fd) {
    while (true) {
        runtime::v1::ClientMessage req;
        std::string err;
        if (!common::read_message(client_fd, req, err)) {
            if (!err.empty())
                std::fprintf(stderr, "shim: read: %s\n", err.c_str());
            break;
        }

        switch (req.msg_case()) {
            case runtime::v1::ClientMessage::kList: {
                // Status через ListRequest.
                runtime::v1::ServerMessage resp;
                resp.set_request_id(req.request_id());
                runtime::v1::ContainerState st;
                int32_t ec = 0, pid = 0;
                build_status(st, ec, pid);
                resp.mutable_list()->mutable_status()->set_code(
                    runtime::v1::STATUS_OK);
                auto* info = resp.mutable_list()->add_containers();
                info->set_id(id_);
                info->set_pid(pid);
                info->set_state(st);
                info->set_exit_code(ec);
                common::write_message(client_fd, resp, err);
                break;
            }
            case runtime::v1::ClientMessage::kKill: {
                runtime::v1::ServerMessage resp;
                resp.set_request_id(req.request_id());
                if (::kill(pid_, SIGTERM) < 0 && errno != ESRCH) {
                    resp.mutable_kill()->mutable_status()->set_code(
                        runtime::v1::STATUS_INTERNAL);
                    resp.mutable_kill()->mutable_status()->set_message(
                        std::string("kill: ") + std::strerror(errno));
                } else {
                    resp.mutable_kill()->mutable_status()->set_code(
                        runtime::v1::STATUS_OK);
                }
                common::write_message(client_fd, resp, err);
                break;
            }
            case runtime::v1::ClientMessage::kAttach: {
                handle_attach_session(client_fd, req);
                ::close(client_fd);
                return;
            }
            default: {
                runtime::v1::ServerMessage resp;
                resp.set_request_id(req.request_id());
                resp.mutable_list()->mutable_status()->set_code(
                    runtime::v1::STATUS_INVALID_ARGUMENT);
                resp.mutable_list()->mutable_status()->set_message(
                    "unsupported request for shim");
                common::write_message(client_fd, resp, err);
                break;
            }
        }
    }
    ::close(client_fd);
}

int ShimService::run() {
    // Запустить waitpid-поток.
    wait_thread_ = std::thread([this] { wait_thread_fn(); });

    // Открыть сокет.
    std::string err;
    int listen_fd = create_listen_socket(socket_path_, err);
    if (listen_fd < 0) {
        std::fprintf(stderr, "shim: %s\n", err.c_str());
        return 1;
    }

    std::fprintf(stderr, "shim[%s]: listening on %s (pid %d)\n",
                 id_.c_str(), socket_path_.c_str(), pid_);

    // Цикл accept. Демон может переподключаться после рестарта.
    while (true) {
        int client_fd = ::accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "shim: accept: %s\n", std::strerror(errno));
            break;
        }
        handle_client(client_fd);
    }

    ::close(listen_fd);
    ::unlink(socket_path_.c_str());
    return 0;
}


void ShimService::handle_attach_session(int client_fd,
                                        const runtime::v1::ClientMessage& hello) {
    // Если tty=false — PTY нет, attach невозможен.
    if (master_fd_ < 0) {
        runtime::v1::ServerMessage resp;
        resp.set_request_id(hello.request_id());
        resp.mutable_attach()->mutable_exit()->set_exit_code(1);
        std::string err;
        common::write_message(client_fd, resp, err);
        return;
    }

    // epoll на master_fd_ и client_fd.
    int epfd = ::epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        runtime::v1::ServerMessage resp;
        resp.set_request_id(hello.request_id());
        resp.mutable_attach()->mutable_exit()->set_exit_code(1);
        std::string err;
        common::write_message(client_fd, resp, err);
        return;
    }

    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = master_fd_;
    ::epoll_ctl(epfd, EPOLL_CTL_ADD, master_fd_, &ev);

    ev.events = EPOLLIN;
    ev.data.fd = client_fd;
    ::epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev);

    bool done = false;
    bool exited_sent = false;
    char buf[4096];
    struct epoll_event events[2];

    auto send_output = [&](const runtime::v1::AttachOutput& out) -> bool {
        runtime::v1::ServerMessage resp;
        resp.set_request_id(hello.request_id());
        *resp.mutable_attach() = out;
        std::string e;
        return common::write_message(client_fd, resp, e);
    };

    while (!done) {
        // Если процесс завершился — отправить exit и выйти.
        if (exited_.load(std::memory_order_acquire) && !exited_sent) {
            runtime::v1::AttachOutput out;
            out.mutable_exit()->set_exit_code(
                exit_code_.load(std::memory_order_relaxed));
            send_output(out);
            exited_sent = true;
            done = true;
            break;
        }

        int nfds = ::epoll_wait(epfd, events, 2, 100);  // 100 мс таймаут
        if (nfds < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (nfds == 0) continue;  // таймаут — проверить exited_

        for (int i = 0; i < nfds && !done; ++i) {
            int fd = events[i].data.fd;

            if (fd == master_fd_) {
                ssize_t n = ::read(master_fd_, buf, sizeof(buf));
                if (n > 0) {
                    runtime::v1::AttachOutput out;
                    out.set_data(std::string(buf, static_cast<size_t>(n)));
                    if (!send_output(out)) {
                        done = true;
                    }
                } else if (n == 0) {
                    // PTY закрыт — процесс завершился, отправить exit.
                    if (!exited_sent) {
                        runtime::v1::AttachOutput out;
                        out.mutable_exit()->set_exit_code(
                            exited_.load() ? exit_code_.load() : 0);
                        send_output(out);
                        exited_sent = true;
                    }
                    done = true;
                } else if (errno != EINTR) {
                    done = true;
                }
            } else if (fd == client_fd) {
                runtime::v1::ClientMessage msg;
                std::string e;
                if (!common::read_message(client_fd, msg, e)) {
                    done = true;
                    break;
                }
                if (!msg.has_attach()) {
                    done = true;
                    break;
                }
                const auto& in = msg.attach();
                if (in.has_data()) {
                    const std::string& d = in.data();
                    size_t off = 0;
                    while (off < d.size()) {
                        ssize_t w = ::write(master_fd_, d.data() + off, d.size() - off);
                        if (w < 0) {
                            if (errno == EINTR) continue;
                            done = true;
                            break;
                        }
                        off += static_cast<size_t>(w);
                    }
                } else if (in.has_resize()) {
                    struct winsize ws{};
                    ws.ws_row = static_cast<unsigned short>(in.resize().rows());
                    ws.ws_col = static_cast<unsigned short>(in.resize().cols());
                    ::ioctl(master_fd_, TIOCSWINSZ, &ws);
                } else if (in.has_eof() && in.eof()) {
                    // Закрыть stdin контейнера: послать EOT в PTY.
                    char eot = 0x04;
                    ::write(master_fd_, &eot, 1);
                }
            }
        }
    }

    ::close(epfd);
}

}  // namespace shim
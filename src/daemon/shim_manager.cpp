#include "shim_manager.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>

#include "framing.hpp"
#include "path.hpp"

namespace cont_daemon {
namespace {

runtime::v1::Status ok_status() {
    runtime::v1::Status s;
    s.set_code(runtime::v1::STATUS_OK);
    return s;
}

runtime::v1::Status error_status(runtime::v1::StatusCode code,
                                 std::string message) {
    runtime::v1::Status s;
    s.set_code(code);
    s.set_message(std::move(message));
    return s;
}

/*class StubShimManager : public ShimManager {
public:
    runtime::v1::Status start(const runtime::v1::ContainerStateFile& state,
                              int32_t& out_pid,
                              int32_t& out_shim_pid) override {
        std::lock_guard lock(mu_);
        const auto& id = state.info().id();
        if (containers_.contains(id)) {
            return error_status(runtime::v1::STATUS_INVALID_ARGUMENT,
                                "container '" + id + "' already started");
        }

        int32_t fake_pid = static_cast<int32_t>(::getpid()) + 1000 +
                           static_cast<int32_t>(containers_.size());

        StubContainer c;
        c.state = runtime::v1::CONTAINER_STATE_RUNNING;
        c.pid = fake_pid;
        c.exit_code = 0;
        containers_.emplace(id, c);

        out_pid = fake_pid;
        out_shim_pid = 0;
        return ok_status();
    }

    runtime::v1::Status signal(const std::string& id) override {
        std::lock_guard lock(mu_);
        auto it = containers_.find(id);
        if (it == containers_.end()) {
            return error_status(runtime::v1::STATUS_NOT_FOUND,
                                "container '" + id + "' not found");
        }
        if (it->second.state != runtime::v1::CONTAINER_STATE_RUNNING) {
            return error_status(runtime::v1::STATUS_FAILED_PRECONDITION,
                                "container '" + id + "' is not running");
        }
        it->second.state = runtime::v1::CONTAINER_STATE_STOPPED;
        it->second.exit_code = 0;
        return ok_status();
    }

    runtime::v1::Status status(const std::string& id,
                               runtime::v1::ContainerState& out_state,
                               int32_t& out_exit_code,
                               int32_t& out_pid) override {
        std::lock_guard lock(mu_);
        auto it = containers_.find(id);
        if (it == containers_.end()) {
            return error_status(runtime::v1::STATUS_NOT_FOUND,
                                "container '" + id + "' not found");
        }
        out_state = it->second.state;
        out_exit_code = it->second.exit_code;
        out_pid = it->second.pid;
        return ok_status();
    }

    void attach(const std::string& id,
                OutputCallback on_output,
                std::function<bool(runtime::v1::AttachInput&)> next_input) override {
        {
            std::lock_guard lock(mu_);
            auto it = containers_.find(id);
            if (it == containers_.end()) {
                runtime::v1::AttachOutput out;
                out.mutable_exit()->set_exit_code(1);
                on_output(out);
                return;
            }
            if (it->second.state != runtime::v1::CONTAINER_STATE_RUNNING) {
                runtime::v1::AttachOutput out;
                out.mutable_exit()->set_exit_code(it->second.exit_code);
                on_output(out);
                return;
            }
        }

        for (;;) {
            runtime::v1::AttachInput in;
            if (!next_input(in)) {
                break;
            }

            if (in.has_eof() && in.eof()) {
                runtime::v1::AttachOutput out;
                out.mutable_exit()->set_exit_code(0);
                on_output(out);
                return;
            }

            if (in.has_data()) {
                runtime::v1::AttachOutput out;
                out.set_data(in.data());
                on_output(out);
            }
        }

        runtime::v1::AttachOutput out;
        out.mutable_exit()->set_exit_code(0);
        on_output(out);
    }

private:
    struct StubContainer {
        runtime::v1::ContainerState state = runtime::v1::CONTAINER_STATE_UNSPECIFIED;
        int32_t pid = 0;
        int32_t exit_code = 0;
    };

    mutable std::mutex mu_;
    std::unordered_map<std::string, StubContainer> containers_;
};*/


// Подключиться к Unix-сокету с retry: shim может ещё не успеть listen.
int connect_with_retry(const std::string& path, int attempts, int delay_ms) {
    for (int i = 0; i < attempts; ++i) {
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (path.size() >= sizeof(addr.sun_path)) {
            ::close(fd);
            return -1;
        }
        std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            return fd;
        }
        ::close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
    return -1;
}

// Один unary-обмен: отправить ClientMessage, получить ServerMessage.
bool shim_request(int fd,
                  runtime::v1::ClientMessage& req,
                  runtime::v1::ServerMessage& resp,
                  std::string& err) {
    if (!common::write_message(fd, req, err)) return false;
    if (!common::read_message(fd, resp, err)) return false;
    return true;
}

class RealShimManager : public ShimManager {
public:
    explicit RealShimManager(std::string shim_path)
        : shim_path_(std::move(shim_path)) {}

    runtime::v1::Status start(const runtime::v1::ContainerStateFile& state,
                              int32_t& out_pid,
                              int32_t& out_shim_pid) override {
        const std::string& id = state.info().id();

        std::vector<std::string> args;
        args.push_back(shim_path_);
        args.push_back("--id");
        args.push_back(id);
        args.push_back("--rootfs");
        args.push_back(state.info().rootfs());
        args.push_back("--socket");
        args.push_back(state.shim_socket());
        if (!state.log_path().empty()) {
            args.push_back("--log");
            args.push_back(state.log_path());
        }
        if (state.info().tty()) {
            args.push_back("--tty");
        }
        args.push_back("--");
        for (const auto& a : state.info().argv()) {
            args.push_back(a);
        }

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (auto& s : args) argv.push_back(s.data());
        argv.push_back(nullptr);

        pid_t pid = ::fork();
        if (pid < 0) {
            return error_status(runtime::v1::STATUS_INTERNAL,
                                std::string("fork: ") + std::strerror(errno));
        }

        if (pid == 0) {
            ::setsid();
            ::execv(shim_path_.c_str(), argv.data());
            ::_exit(127);
        }

        int fd = connect_with_retry(state.shim_socket(),
                                    /*attempts=*/50, /*delay_ms=*/20);
        if (fd < 0) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
            return error_status(runtime::v1::STATUS_INTERNAL,
                                "shim did not open socket: " + state.shim_socket());
        }

        runtime::v1::ClientMessage req;
        req.set_request_id(1);
        req.mutable_list()->set_include_stopped(true);

        runtime::v1::ServerMessage resp;
        std::string err;
        bool ok = shim_request(fd, req, resp, err);
        ::close(fd);

        if (!ok) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
            return error_status(runtime::v1::STATUS_INTERNAL,
                                "shim status: " + err);
        }

        if (!resp.has_list() ||
            resp.list().status().code() != runtime::v1::STATUS_OK ||
            resp.list().containers_size() == 0) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
            return error_status(runtime::v1::STATUS_INTERNAL,
                                "shim returned bad status");
        }

        out_pid = resp.list().containers(0).pid();
        out_shim_pid = pid;
        return ok_status();
    }

    runtime::v1::Status signal(const std::string& id) override {
        const std::string socket_path = common::container_shim_socket(id).string();

        int fd = connect_with_retry(socket_path, /*attempts=*/5, /*delay_ms=*/20);
        if (fd < 0) {
            return error_status(runtime::v1::STATUS_NOT_FOUND,
                                "shim socket not found for '" + id + "'");
        }

        runtime::v1::ClientMessage req;
        req.set_request_id(1);
        req.mutable_kill()->set_id(id);

        runtime::v1::ServerMessage resp;
        std::string err;
        bool ok = shim_request(fd, req, resp, err);
        ::close(fd);

        if (!ok) {
            return error_status(runtime::v1::STATUS_INTERNAL, "shim signal: " + err);
        }
        if (!resp.has_kill()) {
            return error_status(runtime::v1::STATUS_INTERNAL, "shim unexpected response");
        }
        return resp.kill().status();
    }

    runtime::v1::Status status(const std::string& id,
                               runtime::v1::ContainerState& out_state,
                               int32_t& out_exit_code,
                               int32_t& out_pid) override {
        const std::string socket_path = common::container_shim_socket(id).string();

        int fd = connect_with_retry(socket_path, /*attempts=*/5, /*delay_ms=*/20);
        if (fd < 0) {
            return error_status(runtime::v1::STATUS_NOT_FOUND,
                                "shim socket not found for '" + id + "'");
        }

        runtime::v1::ClientMessage req;
        req.set_request_id(1);
        req.mutable_list()->set_include_stopped(true);

        runtime::v1::ServerMessage resp;
        std::string err;
        bool ok = shim_request(fd, req, resp, err);
        ::close(fd);

        if (!ok) {
            return error_status(runtime::v1::STATUS_INTERNAL, "shim status: " + err);
        }
        if (!resp.has_list() ||
            resp.list().status().code() != runtime::v1::STATUS_OK ||
            resp.list().containers_size() == 0) {
            return error_status(runtime::v1::STATUS_INTERNAL, "shim returned bad status");
        }

        const auto& info = resp.list().containers(0);
        out_state = info.state();
        out_exit_code = info.exit_code();
        out_pid = info.pid();
        return ok_status();
    }

    void attach(const std::string& id,
                int client_fd,
                OutputCallback send_to_client) {
        const std::string socket_path = common::container_shim_socket(id).string();

        int shim_fd = connect_with_retry(socket_path, /*attempts=*/5, /*delay_ms=*/20);
        if (shim_fd < 0) {
            runtime::v1::AttachOutput out;
            out.mutable_exit()->set_exit_code(1);
            send_to_client(out);
            return;
        }

        {
            runtime::v1::ClientMessage hello;
            hello.set_request_id(1);
            hello.mutable_attach()->set_id(id);
            std::string err;
            if (!common::write_message(shim_fd, hello, err)) {
                ::close(shim_fd);
                runtime::v1::AttachOutput out;
                out.mutable_exit()->set_exit_code(1);
                send_to_client(out);
                return;
            }
        }

        int epfd = ::epoll_create1(EPOLL_CLOEXEC);
        if (epfd < 0) {
            ::close(shim_fd);
            runtime::v1::AttachOutput out;
            out.mutable_exit()->set_exit_code(1);
            send_to_client(out);
            return;
        }

        struct epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = shim_fd;
        ::epoll_ctl(epfd, EPOLL_CTL_ADD, shim_fd, &ev);

        ev.events = EPOLLIN;
        ev.data.fd = client_fd;
        ::epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev);

        bool done = false;
        struct epoll_event events[2];

        while (!done) {
            int nfds = ::epoll_wait(epfd, events, 2, -1);
            if (nfds < 0) {
                if (errno == EINTR) continue;
                break;
            }

            for (int i = 0; i < nfds && !done; ++i) {
                int fd = events[i].data.fd;

                if (fd == shim_fd) {
                    runtime::v1::ServerMessage resp;
                    std::string err;
                    if (!common::read_message(shim_fd, resp, err)) {
                        done = true;
                        break;
                    }
                    if (!resp.has_attach()) continue;

                    if (!send_to_client(resp.attach())) {
                        done = true;
                        break;
                    }
                    if (resp.attach().has_exit()) {
                        done = true;
                        break;
                    }
                } else if (fd == client_fd) {
                    runtime::v1::ClientMessage msg;
                    std::string err;
                    if (!common::read_message(client_fd, msg, err)) {
                        done = true;
                        break;
                    }
                    if (!msg.has_attach()) {
                        done = true;
                        break;
                    }

                    runtime::v1::ClientMessage fwd;
                    fwd.set_request_id(1);
                    *fwd.mutable_attach() = msg.attach();
                    if (!common::write_message(shim_fd, fwd, err)) {
                        done = true;
                        break;
                    }
                }
            }
        }

        ::close(epfd);
        ::shutdown(shim_fd, SHUT_RDWR);
        ::close(shim_fd);
    }

private:
    std::string shim_path_;
};

}  // namespace

/*std::unique_ptr<ShimManager> make_stub_shim_manager() {
    return std::make_unique<StubShimManager>();
}*/

std::unique_ptr<ShimManager> make_real_shim_manager(std::string shim_path) {
    return std::make_unique<RealShimManager>(std::move(shim_path));
}

}  // namespace cont_daemon
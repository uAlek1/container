#include "runtime_service.hpp"

#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <thread>

#include "path.hpp"

namespace cont_daemon {
namespace {

std::string generate_id() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t v = dist(rng);
    char buf[13];
    std::snprintf(buf, sizeof(buf), "%012llx",
                  static_cast<unsigned long long>(v & 0xFFFFFFFFFFFFULL));
    return std::string(buf);
}

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

off_t find_tail_offset(int fd, int64_t tail_lines) {
    if (tail_lines <= 0) return 0;

    off_t size = ::lseek(fd, 0, SEEK_END);
    if (size <= 0) return 0;

    constexpr size_t kBlock = 4096;
    char buf[kBlock];
    int64_t lines = 0;
    off_t pos = size;

    while (pos > 0) {
        size_t to_read = std::min<size_t>(kBlock, static_cast<size_t>(pos));
        pos -= static_cast<off_t>(to_read);
        if (::lseek(fd, pos, SEEK_SET) < 0) return 0;
        ssize_t n = ::read(fd, buf, to_read);
        if (n <= 0) return 0;

        for (ssize_t i = n - 1; i >= 0; --i) {
            if (buf[i] == '\n') {
                ++lines;
                if (lines > tail_lines) {
                    return pos + i + 1;
                }
            }
        }
    }
    return 0;
}

}  // namespace

RuntimeService::RuntimeService(ContainerRegistry& registry, ShimManager& shim)
    : registry_(registry), shim_(shim) {}

bool RuntimeService::send_one(const SendFn& send, uint64_t request_id,
                              const runtime::v1::ServerMessage& msg) {
    runtime::v1::ServerMessage out = msg;
    out.set_request_id(request_id);
    return send(out);
}

bool RuntimeService::handle(const runtime::v1::ClientMessage& req,
                            int client_fd,
                            const SendFn& send) {
    switch (req.msg_case()) {
        case runtime::v1::ClientMessage::kCreate:
            return handle_create(req, send);
        case runtime::v1::ClientMessage::kList:
            return handle_list(req, send);
        case runtime::v1::ClientMessage::kKill:
            return handle_kill(req, send);
        case runtime::v1::ClientMessage::kLogs:
            return handle_logs(req, send);
        case runtime::v1::ClientMessage::kAttach:
            return handle_attach(req, client_fd, send);
        case runtime::v1::ClientMessage::MSG_NOT_SET:
        default: {
            runtime::v1::ServerMessage out;
            out.mutable_list()->mutable_status()->set_code(
                runtime::v1::STATUS_INVALID_ARGUMENT);
            out.mutable_list()->mutable_status()->set_message(
                "empty ClientMessage (no oneof set)");
            send_one(send, req.request_id(), out);
            return false;
        }
    }
}

bool RuntimeService::handle_create(const runtime::v1::ClientMessage& req,
                                   const SendFn& send) {
    const auto& cr = req.create();

    runtime::v1::ServerMessage out;
    auto* resp = out.mutable_create();

    if (cr.rootfs().empty()) {
        *resp->mutable_status() = error_status(
            runtime::v1::STATUS_INVALID_ARGUMENT, "rootfs is empty");
        send_one(send, req.request_id(), out);
        return true;
    }
    if (cr.argv().empty()) {
        *resp->mutable_status() = error_status(
            runtime::v1::STATUS_INVALID_ARGUMENT, "argv is empty");
        send_one(send, req.request_id(), out);
        return true;
    }
    if (!cr.cwd().empty() && cr.cwd().front() != '/') {
        *resp->mutable_status() = error_status(
            runtime::v1::STATUS_INVALID_ARGUMENT,
            "cwd must be an absolute path");
        send_one(send, req.request_id(), out);
        return true;
    }

    std::string id = generate_id();

    runtime::v1::ContainerStateFile state;
    auto* info = state.mutable_info();
    info->set_id(id);
    info->set_rootfs(cr.rootfs());
    for (const auto& a : cr.argv()) 
        info->add_argv(a);
    info->set_tty(cr.tty());
    info->set_detached(cr.detached());
    info->set_state(runtime::v1::CONTAINER_STATE_CREATED);
    info->set_pid(0);
    info->set_created_at(static_cast<int64_t>(
        std::chrono::system_clock::now().time_since_epoch().count() / 1000000000));
    info->set_exit_code(0);
    state.set_shim_socket(common::container_shim_socket(id).string());
    state.set_log_path(cr.tty() ? "" : common::container_log_file(id).string());
    state.set_shim_pid(0);

    // Зарегистрировать (пишет state.pb на диск).
    std::string err;
    if (!registry_.add(state, err)) {
        *resp->mutable_status() = error_status(
            runtime::v1::STATUS_INTERNAL, "register: " + err);
        send_one(send, req.request_id(), out);
        return true;
    }

    int32_t pid = 0, shim_pid = 0;
    auto shim_status = shim_.start(state, pid, shim_pid);
    if (shim_status.code() != runtime::v1::STATUS_OK) {
        registry_.remove(id, err);
        *resp->mutable_status() = shim_status;
        send_one(send, req.request_id(), out);
        return true;
    }

    // Обновить state: RUNNING, реальные pid'ы.
    if (!registry_.set_state(id, runtime::v1::CONTAINER_STATE_RUNNING,
                             /*exit_code=*/0, err)) {
        *resp->mutable_status() = error_status(
            runtime::v1::STATUS_INTERNAL, "set_state: " + err);
        send_one(send, req.request_id(), out);
        return true;
    }
    auto updated = registry_.get(id);
    if (updated) {
        updated->mutable_info()->set_pid(pid);
        updated->set_shim_pid(shim_pid);
        registry_.update(*updated, err);
    }

    // Ответить успехом.
    *resp->mutable_status() = ok_status();
    resp->set_id(id);
    if (updated) {
        *resp->mutable_info() = updated->info();
    } else {
        *resp->mutable_info() = info[0];  // fallback, не должно случиться
    }
    send_one(send, req.request_id(), out);
    return true;
}

// ============================================================
// List
// ============================================================

bool RuntimeService::handle_list(const runtime::v1::ClientMessage& req,
                                 const SendFn& send) {
    runtime::v1::ServerMessage out;
    auto* resp = out.mutable_list();

    auto states = registry_.list(req.list().include_stopped());
    *resp->mutable_status() = ok_status();
    for (const auto& s : states) {
        *resp->add_containers() = s.info();
    }

    send_one(send, req.request_id(), out);
    return true;
}

// ============================================================
// Kill
// ============================================================

bool RuntimeService::handle_kill(const runtime::v1::ClientMessage& req,
                                 const SendFn& send) {
    runtime::v1::ServerMessage out;
    auto* resp = out.mutable_kill();

    const auto& id = req.kill().id();
    if (id.empty()) {
        *resp->mutable_status() = error_status(
            runtime::v1::STATUS_INVALID_ARGUMENT, "id is empty");
        send_one(send, req.request_id(), out);
        return true;
    }

    auto state = registry_.get(id);
    if (!state) {
        *resp->mutable_status() = error_status(
            runtime::v1::STATUS_NOT_FOUND, "container '" + id + "' not found");
        send_one(send, req.request_id(), out);
        return true;
    }

    auto shim_status = shim_.signal(id);
    if (shim_status.code() != runtime::v1::STATUS_OK) {
        *resp->mutable_status() = shim_status;
        send_one(send, req.request_id(), out);
        return true;
    }

    std::string err;
    if (!registry_.set_state(id, runtime::v1::CONTAINER_STATE_STOPPED,
                             /*exit_code=*/0, err)) {
        *resp->mutable_status() = error_status(
            runtime::v1::STATUS_INTERNAL, "set_state: " + err);
        send_one(send, req.request_id(), out);
        return true;
    }

    *resp->mutable_status() = ok_status();
    send_one(send, req.request_id(), out);
    return true;
}

bool RuntimeService::handle_logs(const runtime::v1::ClientMessage& req,
                                 const SendFn& send) {
    const auto& lr = req.logs();
    const uint64_t rid = req.request_id();

    auto state = registry_.get(lr.id());
    if (!state) {
        runtime::v1::ServerMessage out;
        out.set_request_id(rid);
        *out.mutable_log()->mutable_status() = error_status(
            runtime::v1::STATUS_NOT_FOUND, "container '" + lr.id() + "' not found");
        out.mutable_log()->set_eof(true);
        send(out);
        return false;
    }

    if (state->info().tty()) {
        runtime::v1::ServerMessage out;
        out.set_request_id(rid);
        *out.mutable_log()->mutable_status() = error_status(
            runtime::v1::STATUS_FAILED_PRECONDITION,
            "container was started with tty, logs unavailable");
        out.mutable_log()->set_eof(true);
        send(out);
        return false;
    }

    const std::string log_path = state->log_path();
    int fd = ::open(log_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        runtime::v1::ServerMessage out;
        out.set_request_id(rid);
        *out.mutable_log()->mutable_status() = error_status(
            runtime::v1::STATUS_NOT_FOUND,
            "log file not found: " + log_path);
        out.mutable_log()->set_eof(true);
        send(out);
        return false;
    }

    off_t offset = find_tail_offset(fd, lr.tail_lines());
    if (::lseek(fd, offset, SEEK_SET) < 0) {
        ::close(fd);
        runtime::v1::ServerMessage out;
        out.set_request_id(rid);
        *out.mutable_log()->mutable_status() = error_status(
            runtime::v1::STATUS_INTERNAL, "lseek failed");
        out.mutable_log()->set_eof(true);
        send(out);
        return false;
    }

    constexpr size_t kChunk = 32 * 1024;
    std::string buf(kChunk, '\0');
    bool follow = lr.follow();

    auto send_chunk = [&](const char* data, size_t len) -> bool {
        runtime::v1::ServerMessage out;
        out.set_request_id(rid);
        auto* chunk = out.mutable_log();
        chunk->mutable_status()->set_code(runtime::v1::STATUS_OK);
        if (len > 0) chunk->set_data(data, len);
        return send(out);
    };

    for (;;) {
        ssize_t n = ::read(fd, buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            runtime::v1::ServerMessage out;
            out.set_request_id(rid);
            *out.mutable_log()->mutable_status() = error_status(
                runtime::v1::STATUS_INTERNAL, "read failed");
            out.mutable_log()->set_eof(true);
            send(out);
            return false;
        }

        if (n > 0) {
            if (!send_chunk(buf.data(), static_cast<size_t>(n))) {
                ::close(fd);
                return false;
            }
            continue;
        }

        if (!follow) break;

        auto st = registry_.get(lr.id());
        if (!st) break;

        if (st->info().state() != runtime::v1::CONTAINER_STATE_RUNNING) {
            for (int i = 0; i < 10; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                ssize_t m = ::read(fd, buf.data(), buf.size());
                if (m > 0) {
                    if (!send_chunk(buf.data(), static_cast<size_t>(m))) {
                        ::close(fd);
                        return false;
                    }
                } else if (m == 0) {
                    ::close(fd);
                    runtime::v1::ServerMessage out;
                    out.set_request_id(rid);
                    out.mutable_log()->mutable_status()->set_code(runtime::v1::STATUS_OK);
                    out.mutable_log()->set_eof(true);
                    send(out);
                    return false;
                }
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ::close(fd);

    runtime::v1::ServerMessage out;
    out.set_request_id(rid);
    out.mutable_log()->mutable_status()->set_code(runtime::v1::STATUS_OK);
    out.mutable_log()->set_eof(true);
    send(out);
    return false;
}

bool RuntimeService::handle_attach(const runtime::v1::ClientMessage& req,
                                   int client_fd,
                                   const SendFn& send) {
    
    const std::string id = req.attach().id();
    const uint64_t rid = req.request_id();

    if (id.empty()) {
        runtime::v1::ServerMessage out;
        out.set_request_id(rid);
        out.mutable_attach()->mutable_exit()->set_exit_code(1);
        send(out);
        return false;
    }

    auto state = registry_.get(id);
    if (!state) {
        runtime::v1::ServerMessage out;
        out.set_request_id(rid);
        out.mutable_attach()->mutable_exit()->set_exit_code(1);
        send(out);
        return false;
    }

    auto send_to_client = [&](const runtime::v1::AttachOutput& out_msg) -> bool {
        runtime::v1::ServerMessage out;
        out.set_request_id(rid);
        *out.mutable_attach() = out_msg;
        return send(out);
    };

    shim_.attach(id, client_fd, send_to_client);
    return false;
}

}  // namespace daemon
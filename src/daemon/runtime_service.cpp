#include "runtime_service.hpp"

#include <chrono>
#include <cstdio>
#include <random>
#include <string>

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
            return handle_attach(req, send);
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

// ============================================================
// Logs / Attach — пока заглушки
// ============================================================

bool RuntimeService::handle_logs(const runtime::v1::ClientMessage& req,
                                 const SendFn& send) {
    runtime::v1::ServerMessage out;
    auto* chunk = out.mutable_log();
    *chunk->mutable_status() = error_status(
        runtime::v1::STATUS_FAILED_PRECONDITION,
        "logs not implemented yet");
    chunk->set_eof(true);
    send_one(send, req.request_id(), out);
    return false;
}

bool RuntimeService::handle_attach(const runtime::v1::ClientMessage& req,
                                   const SendFn& send) {
    runtime::v1::ServerMessage out;
    auto* att = out.mutable_attach();
    att->mutable_exit()->set_exit_code(1);
    send_one(send, req.request_id(), out);
    return false;
}

}  // namespace daemon
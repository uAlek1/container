#pragma once

#include <functional>
#include <string>

#include "registry.hpp"
#include "shim_manager.hpp"
#include "runtime.pb.h"

namespace cont_daemon {

class RuntimeService {
public:
    using SendFn = std::function<bool(const runtime::v1::ServerMessage&)>;
    using RecvFn = std::function<bool(runtime::v1::ClientMessage&)>;


    RuntimeService(ContainerRegistry& registry, ShimManager& shim);

    RuntimeService(const RuntimeService&) = delete;
    RuntimeService& operator=(const RuntimeService&) = delete;

    bool handle(const runtime::v1::ClientMessage& req, int client_fd, const SendFn& send);

private:
    bool handle_create(const runtime::v1::ClientMessage& req, const SendFn& send);
    bool handle_list(const runtime::v1::ClientMessage& req, const SendFn& send);
    bool handle_kill(const runtime::v1::ClientMessage& req, const SendFn& send);
    bool handle_logs(const runtime::v1::ClientMessage& req, const SendFn& send);
    bool handle_attach(const runtime::v1::ClientMessage& req,
                   int client_fd,
                   const SendFn& send);

    static bool send_one(const SendFn& send, uint64_t request_id,
                         const runtime::v1::ServerMessage& msg);

    ContainerRegistry& registry_;
    ShimManager& shim_;
};

}  // namespace daemon
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "runtime.pb.h"

namespace shim {

class ShimService {
public:
    ShimService(std::string id, int32_t pid, int master_fd, std::string socket_path);
    ~ShimService();

    ShimService(const ShimService&) = delete;
    ShimService& operator=(const ShimService&) = delete;

    int run();

private:
    void wait_thread_fn();
    void handle_client(int client_fd);

    runtime::v1::Status build_status(runtime::v1::ContainerState& out_state,
                                     int32_t& out_exit_code,
                                     int32_t& out_pid);

    void handle_attach_session(int client_fd,
                               const runtime::v1::ClientMessage& hello);

    std::string id_;
    int32_t pid_;
    int master_fd_;
    std::string socket_path_;

    std::atomic<int32_t> exit_code_{-1};   // -1 = ещё жив
    std::atomic<bool>    exited_{false};

    std::thread wait_thread_;
};

}  // namespace shim
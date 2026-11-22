#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "runtime.pb.h"

namespace cont_daemon {

class ShimManager {
public:
    virtual ~ShimManager() = default;

    virtual runtime::v1::Status start(const runtime::v1::ContainerStateFile& state,
                                      int32_t& out_pid,
                                      int32_t& out_shim_pid) = 0;


    virtual runtime::v1::Status signal(const std::string& id) = 0;

    virtual runtime::v1::Status status(const std::string& id,
                                       runtime::v1::ContainerState& out_state,
                                       int32_t& out_exit_code,
                                       int32_t& out_pid) = 0;

    using OutputCallback = std::function<void(const runtime::v1::AttachOutput&)>;
    virtual void attach(const std::string& id,
                        OutputCallback on_output,
                        std::function<bool(runtime::v1::AttachInput&)> next_input) = 0;
};

std::unique_ptr<ShimManager> make_stub_shim_manager();

}  // namespace daemon
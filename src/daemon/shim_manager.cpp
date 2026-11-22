#include "shim_manager.hpp"

#include <mutex>
#include <unordered_map>

#include <unistd.h>

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

class StubShimManager : public ShimManager {
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
        out_shim_pid = 0;  // заглушка не создаёт shim-процесс
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
};

}  // namespace

std::unique_ptr<ShimManager> make_stub_shim_manager() {
    return std::make_unique<StubShimManager>();
}

}  // namespace daemon
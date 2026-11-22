#include "state_poller.hpp"

#include <cstdio>

namespace cont_daemon {

StatePoller::StatePoller(ContainerRegistry& registry,
                         ShimManager& shim,
                         std::chrono::milliseconds interval)
    : registry_(registry), shim_(shim), interval_(interval) {}

StatePoller::~StatePoller() {
    stop();
}

void StatePoller::start() {
    if (thread_.joinable()) return;
    stop_.store(false, std::memory_order_relaxed);
    thread_ = std::thread([this] { run(); });
}

void StatePoller::stop() {
    stop_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) thread_.join();
}

void StatePoller::run() {
    while (!stop_.load(std::memory_order_relaxed)) {
        auto states = registry_.list(/*include_stopped=*/false);

        for (const auto& s : states) {
            if (stop_.load(std::memory_order_relaxed)) break;

            const std::string& id = s.info().id();
            runtime::v1::ContainerState shim_state;
            int32_t exit_code = 0, pid = 0;

            auto st = shim_.status(id, shim_state, exit_code, pid);
            std::string err;

            if (st.code() != runtime::v1::STATUS_OK) {
                registry_.set_state(id,
                                    runtime::v1::CONTAINER_STATE_EXITED,
                                    /*exit_code=*/-1, err);
            } else if (shim_state != runtime::v1::CONTAINER_STATE_RUNNING) {
                registry_.set_state(id, shim_state, exit_code, err);
            }
        }
        std::this_thread::sleep_for(interval_);
    }
}

}  // namespace daemon
#pragma once

#include <atomic>
#include <chrono>
#include <thread>

#include "registry.hpp"
#include "shim_manager.hpp"

namespace cont_daemon {

class StatePoller {
public:
    StatePoller(ContainerRegistry& registry,
                ShimManager& shim,
                std::chrono::milliseconds interval);
    ~StatePoller();

    StatePoller(const StatePoller&) = delete;
    StatePoller& operator=(const StatePoller&) = delete;

    void start();
    void stop();

private:
    void run();

    ContainerRegistry& registry_;
    ShimManager& shim_;
    std::chrono::milliseconds interval_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace daemon
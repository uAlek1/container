#include "recovery.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include <csignal>
#include <unistd.h>

#include "path.hpp"
#include "state_store.hpp"

namespace cont_daemon {
namespace {

bool pid_alive(int32_t pid) {
    if (pid <= 0) return false;
    if (::kill(pid, 0) == 0) return true;
    return errno == EPERM; 
}

bool recover_one(ContainerRegistry& registry,
                 ShimManager& shim,
                 const std::filesystem::path& dir) {
    std::string err;
    auto state_opt = common::load_state(dir, err);
    if (!state_opt) {
        std::fprintf(stderr, "recovery: skip %s: %s\n",
                     dir.c_str(), err.c_str());
        return false;
    }

    runtime::v1::ContainerStateFile state = std::move(*state_opt);
    const std::string& id = state.info().id();
    if (id.empty()) {
        std::fprintf(stderr, "recovery: skip %s: empty id in state.pb\n",
                     dir.c_str());
        return false;
    }

    const auto current = state.info().state();

    if (current == runtime::v1::CONTAINER_STATE_STOPPED ||
        current == runtime::v1::CONTAINER_STATE_EXITED) {
        if (!registry.add(state, err)) {
            std::fprintf(stderr, "recovery: add %s: %s\n", id.c_str(), err.c_str());
            return false;
        }
        return true;
    }

    int32_t shim_pid = state.shim_pid();
    if (!pid_alive(shim_pid)) {
        std::fprintf(stderr, "recovery: %s: shim pid %d is dead, marking EXITED\n",
                     id.c_str(), shim_pid);
        state.mutable_info()->set_state(runtime::v1::CONTAINER_STATE_EXITED);
        state.mutable_info()->set_exit_code(-1);

        std::error_code ec;
        std::filesystem::remove(state.shim_socket(), ec);

        if (!registry.add(state, err)) {
            std::fprintf(stderr, "recovery: add %s: %s\n", id.c_str(), err.c_str());
            return false;
        }
        return true;
    }

    runtime::v1::ContainerState shim_state;
    int32_t exit_code = 0, pid = 0;
    auto st = shim.status(id, shim_state, exit_code, pid);

    if (st.code() != runtime::v1::STATUS_OK) {
        std::fprintf(stderr, "recovery: %s: shim status failed: %s, marking EXITED\n",
                     id.c_str(), st.message().c_str());
        state.mutable_info()->set_state(runtime::v1::CONTAINER_STATE_EXITED);
        state.mutable_info()->set_exit_code(-1);

        std::error_code ec;
        std::filesystem::remove(state.shim_socket(), ec);

        if (!registry.add(state, err)) {
            std::fprintf(stderr, "recovery: add %s: %s\n", id.c_str(), err.c_str());
            return false;
        }
        return true;
    }

    state.mutable_info()->set_state(shim_state);
    state.mutable_info()->set_exit_code(exit_code);
    if (pid > 0) state.mutable_info()->set_pid(pid);

    if (!registry.add(state, err)) {
        std::fprintf(stderr, "recovery: add %s: %s\n", id.c_str(), err.c_str());
        return false;
    }

    std::fprintf(stderr, "recovery: %s: restored as state=%d pid=%d exit=%d\n",
                 id.c_str(), static_cast<int>(shim_state), pid, exit_code);
    return true;
}

}  // namespace

int recover_containers(ContainerRegistry& registry, ShimManager& shim) {
    const auto root = common::state_root();

    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) {
        std::fprintf(stderr, "recovery: state root %s does not exist, nothing to recover\n",
                     root.c_str());
        return 0;
    }

    int recovered = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (ec) break;
        if (!entry.is_directory()) continue;

        // Директория контейнера должна содержать state.pb
        const auto state_file = entry.path() / "state.pb";
        if (!std::filesystem::exists(state_file, ec)) continue;

        if (recover_one(registry, shim, entry.path())) {
            ++recovered;
        }
    }

    return recovered;
}

}  // namespace cont_daemon
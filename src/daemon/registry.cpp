#include "registry.hpp"

#include "path.hpp"
#include "state_store.hpp"

namespace cont_daemon {
namespace {

bool is_stopped(runtime::v1::ContainerState s) {
    return s == runtime::v1::CONTAINER_STATE_STOPPED ||
           s == runtime::v1::CONTAINER_STATE_EXITED;
}

}  // namespace

bool ContainerRegistry::add(const runtime::v1::ContainerStateFile& state,
                            std::string& err) {
    err.clear();
    if (state.info().id().empty()) {
        err = "add: empty container id";
        return false;
    }

    std::lock_guard lock(mu_);
    if (map_.contains(state.info().id())) {
        err = "add: container '" + state.info().id() + "' already exists";
        return false;
    }

    if (!persist_locked(state, err)) {
        return false;
    }

    map_.emplace(state.info().id(), state);
    return true;
}

bool ContainerRegistry::update(const runtime::v1::ContainerStateFile& state,
                               std::string& err) {
    err.clear();
    if (state.info().id().empty()) {
        err = "update: empty container id";
        return false;
    }

    std::lock_guard lock(mu_);
    auto it = map_.find(state.info().id());
    if (it == map_.end()) {
        err = "update: container '" + state.info().id() + "' not found";
        return false;
    }

    if (!persist_locked(state, err)) {
        return false;
    }

    it->second = state;
    return true;
}

bool ContainerRegistry::set_state(std::string_view id,
                                  runtime::v1::ContainerState new_state,
                                  int32_t exit_code,
                                  std::string& err) {
    err.clear();

    std::lock_guard lock(mu_);
    auto it = map_.find(std::string(id));
    if (it == map_.end()) {
        err = "set_state: container '" + std::string(id) + "' not found";
        return false;
    }

    // Копируем, чтобы persist_locked не держал ссылку на map_ во время записи.
    // На практике это дешёвая копия (килобайты), а запись на диск — редкая.
    runtime::v1::ContainerStateFile updated = it->second;
    updated.mutable_info()->set_state(new_state);
    updated.mutable_info()->set_exit_code(exit_code);

    if (!persist_locked(updated, err)) {
        return false;
    }

    it->second = std::move(updated);
    return true;
}

bool ContainerRegistry::remove(std::string_view id, std::string& err) {
    err.clear();

    std::lock_guard lock(mu_);
    auto it = map_.find(std::string(id));
    if (it == map_.end()) {
        err = "remove: container '" + std::string(id) + "' not found";
        return false;
    }

    // Сначала диск, потом память: если удаление с диска не удалось,
    // запись в памяти остаётся, и состояние консистентно.
    if (!common::remove_state(common::container_state_dir(id), err)) {
        return false;
    }

    map_.erase(it);
    return true;
}

std::optional<runtime::v1::ContainerStateFile>
ContainerRegistry::get(std::string_view id) const {
    std::lock_guard lock(mu_);
    auto it = map_.find(std::string(id));
    if (it == map_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<runtime::v1::ContainerStateFile>
ContainerRegistry::list(bool include_stopped) const {
    std::lock_guard lock(mu_);
    std::vector<runtime::v1::ContainerStateFile> out;
    out.reserve(map_.size());
    for (const auto& [id, state] : map_) {
        if (!include_stopped && is_stopped(state.info().state())) {
            continue;
        }
        out.push_back(state);
    }
    return out;
}

size_t ContainerRegistry::size() const {
    std::lock_guard lock(mu_);
    return map_.size();
}

bool ContainerRegistry::persist_locked(
    const runtime::v1::ContainerStateFile& state, std::string& err) {
    return common::save_state(
        common::container_state_dir(state.info().id()), state, err);
}

}  // namespace daemon
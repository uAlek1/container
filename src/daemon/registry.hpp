#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "runtime.pb.h"

namespace cont_daemon {

class ContainerRegistry {
public:
    ContainerRegistry() = default;

    ContainerRegistry(const ContainerRegistry&) = delete;
    ContainerRegistry& operator=(const ContainerRegistry&) = delete;

    bool add(const runtime::v1::ContainerStateFile& state, std::string& err);

    bool update(const runtime::v1::ContainerStateFile& state, std::string& err);

    bool set_state(std::string_view id,
                   runtime::v1::ContainerState new_state,
                   int32_t exit_code,
                   std::string& err);

    bool remove(std::string_view id, std::string& err);

    std::optional<runtime::v1::ContainerStateFile> get(std::string_view id) const;

    std::vector<runtime::v1::ContainerStateFile> list(bool include_stopped) const;

    size_t size() const;

private:
    bool persist_locked(const runtime::v1::ContainerStateFile& state,
                        std::string& err);

    mutable std::mutex mu_;
    std::unordered_map<std::string, runtime::v1::ContainerStateFile> map_;
};

}  // namespace daemon
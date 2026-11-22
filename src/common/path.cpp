#include "path.hpp"

#include <cstdlib>
#include <string>
#include <system_error>

#include <unistd.h>

namespace common {
namespace {

// Прочитать переменную окружения или вернуть fallback.
std::string env_or(const char* name, std::string fallback) {
    const char* v = std::getenv(name);
    if (v && *v) return std::string(v);
    return fallback;
}

// XDG_RUNTIME_DIR: обычно /run/user/<uid>, доступен только владельцу,
// очищается при выходе из сессии. Если не задан — /tmp/container-runtime-<uid>
// с правами 0700 (см. ensure_dir).
std::filesystem::path xdg_runtime_dir() {
    auto dir = env_or("XDG_RUNTIME_DIR", "");
    if (!dir.empty()) return dir;

    // Fallback: /tmp/container-runtime-<uid>.
    return std::filesystem::path("/tmp") /
           ("container-runtime-" + std::to_string(::getuid()));
}

// XDG_STATE_HOME: обычно ~/.local/state. Если не задан — $HOME/.local/state.
std::filesystem::path xdg_state_home() {
    auto dir = env_or("XDG_STATE_HOME", "");
    if (!dir.empty()) return dir;

    auto home = env_or("HOME", "/tmp");
    return std::filesystem::path(home) / ".local" / "state";
}

// Корень runtime-данных демона. В rootless-режиме это поддиректория
// XDG_RUNTIME_DIR, чтобы сокет и state были доступны только владельцу.
std::filesystem::path runtime_root() {
    return xdg_runtime_dir() / "container-runtime";
}

}  // namespace

std::filesystem::path daemon_socket_path() {
    return runtime_root() / "daemon.sock";
}

std::filesystem::path state_root() {
    return runtime_root() / "containers";
}

std::filesystem::path log_root() {
    return xdg_state_home() / "container-runtime" / "logs";
}

std::filesystem::path container_state_dir(std::string_view id) {
    return state_root() / id;
}

std::filesystem::path container_state_file(std::string_view id) {
    return container_state_dir(id) / "state.pb";
}

std::filesystem::path container_shim_socket(std::string_view id) {
    return container_state_dir(id) / "shim.sock";
}

std::filesystem::path container_log_file(std::string_view id) {
    return log_root() / (std::string(id) + ".log");
}

bool ensure_dir(const std::filesystem::path& path) {
    std::error_code ec;

    std::filesystem::create_directories(path, ec);
    if (ec) return false;

    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace,
        ec);
    return !ec;
}

bool ensure_runtime_dirs() {
    return ensure_dir(runtime_root()) &&
           ensure_dir(state_root()) &&
           ensure_dir(log_root());
}

}  // namespace common
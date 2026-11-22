#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace common {

std::filesystem::path daemon_socket_path();   // /run/container-runtime/daemon.sock
std::filesystem::path state_root();           // /run/container-runtime/containers
std::filesystem::path log_root();             // /var/log/container-runtime

std::filesystem::path container_state_dir(std::string_view id);   // <state_root>/<id>
std::filesystem::path container_state_file(std::string_view id);  // <state_root>/<id>/state.pb
std::filesystem::path container_shim_socket(std::string_view id); // <state_root>/<id>/shim.sock
std::filesystem::path container_log_file(std::string_view id);    // <log_root>/<id>.log

// Создать директорию (и родителей), если её нет.
// Возвращает false при ошибке; true, если директория уже есть или создана.
bool ensure_dir(const std::filesystem::path& path);

// Создать state_root() и log_root(). Вызывается при старте демона.
bool ensure_runtime_dirs();

}  // namespace common
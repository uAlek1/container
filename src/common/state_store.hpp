#pragma once

#include <filesystem>
#include <optional>

#include "runtime.pb.h"

namespace common {

// Сохранить состояние в <dir>/state.pb атомарно (временный файл + rename).
// Возвращает false при ошибке; err — текст.
bool save_state(const std::filesystem::path& dir,
                const runtime::v1::ContainerStateFile& state,
                std::string& err);

// Загрузить состояние из <dir>/state.pb.
// Возвращает std::nullopt при ошибке; err — текст.
std::optional<runtime::v1::ContainerStateFile>
load_state(const std::filesystem::path& dir, std::string& err);

// Удалить <dir>/state.pb. Возвращает true, если файла нет или он удалён.
bool remove_state(const std::filesystem::path& dir, std::string& err);

}  // namespace common
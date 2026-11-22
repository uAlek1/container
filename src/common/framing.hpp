#pragma once

#include <cstdint>
#include <string>

#include <google/protobuf/message.h>

namespace common {

// Прочитать одно length-prefix сообщение из fd.
// Возвращает:
//   true  — сообщение прочитано и распарсено в out
//   false — EOF (чистый, на первом байте длины) или ошибка;
//           err — пустая строка при чистом EOF, иначе текст ошибки.
bool read_message(int fd, google::protobuf::Message& out, std::string& err);

// Записать одно length-prefix сообщение в fd.
// Возвращает true при успехе, false при ошибке (err — текст).
bool write_message(int fd, const google::protobuf::Message& msg, std::string& err);

}  // namespace common
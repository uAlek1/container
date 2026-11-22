#include "framing.hpp"

#include <cerrno>
#include <cstring>

#include <unistd.h>

namespace common {
namespace {

void encode_length(uint32_t len, char out[4]) {
    out[0] = static_cast<char>(len & 0xFF);
    out[1] = static_cast<char>((len >> 8) & 0xFF);
    out[2] = static_cast<char>((len >> 16) & 0xFF);
    out[3] = static_cast<char>((len >> 24) & 0xFF);
}

uint32_t decode_length(const char in[4]) {
    return static_cast<uint32_t>(static_cast<unsigned char>(in[0])) |
           (static_cast<uint32_t>(static_cast<unsigned char>(in[1])) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(in[2])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(in[3])) << 24);
}

// Прочитать ровно n байт. Возвращает:
//   >0 — прочитано байт (может быть меньше n только при EOF)
//    0 — чистый EOF на первом байте
//   -1 — ошибка (errno установлен)
ssize_t read_all(int fd, void* buf, size_t n) {
    auto* p = static_cast<char*>(buf);
    size_t total = 0;
    while (total < n) {
        ssize_t r = ::read(fd, p + total, n - total);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) break;
        total += static_cast<size_t>(r);
    }
    return static_cast<ssize_t>(total);
}

// Записать ровно n байт. Возвращает 0 при успехе, -1 при ошибке.
int write_all(int fd, const void* buf, size_t n) {
    const auto* p = static_cast<const char*>(buf);
    size_t total = 0;
    while (total < n) {
        ssize_t w = ::write(fd, p + total, n - total);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += static_cast<size_t>(w);
    }
    return 0;
}

}  // namespace

bool read_message(int fd, google::protobuf::Message& out, std::string& err) {
    err.clear();

    char len_buf[4];
    ssize_t r = read_all(fd, len_buf, sizeof(len_buf));
    if (r < 0) {
        err = std::string("read(length): ") + std::strerror(errno);
        return false;
    }
    if (r == 0) {
        return false;  // чистый EOF, err пуст
    }
    if (r != static_cast<ssize_t>(sizeof(len_buf))) {
        err = "truncated length prefix (EOF mid-frame)";
        return false;
    }

    uint32_t len = decode_length(len_buf);
    if (len == 0) {
        err = "zero-length message";
        return false;
    }

    std::string body(len, '\0');
    ssize_t got = read_all(fd, body.data(), len);
    if (got < 0) {
        err = std::string("read(body): ") + std::strerror(errno);
        return false;
    }
    if (got != static_cast<ssize_t>(len)) {
        err = "truncated body (EOF mid-message)";
        return false;
    }

    if (!out.ParseFromString(body)) {
        err = "ParseFromString failed (malformed protobuf)";
        return false;
    }

    return true;
}

bool write_message(int fd, const google::protobuf::Message& msg, std::string& err) {
    err.clear();

    std::string body;
    if (!msg.SerializeToString(&body)) {
        err = "SerializeToString failed";
        return false;
    }

    if (body.size() > UINT32_MAX) {
        err = "message too large (> UINT32_MAX)";
        return false;
    }

    char len_buf[4];
    encode_length(static_cast<uint32_t>(body.size()), len_buf);

    if (write_all(fd, len_buf, sizeof(len_buf)) < 0) {
        err = std::string("write(length): ") + std::strerror(errno);
        return false;
    }
    if (write_all(fd, body.data(), body.size()) < 0) {
        err = std::string("write(body): ") + std::strerror(errno);
        return false;
    }

    return true;
}

}  // namespace common
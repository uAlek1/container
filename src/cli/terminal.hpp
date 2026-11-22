#pragma once

#include <termios.h>

#include "client.hpp"

namespace terminal {

class RawMode {
public:
    explicit RawMode(int fd);
    ~RawMode();

    RawMode(const RawMode&) = delete;
    RawMode& operator=(const RawMode&) = delete;

private:
    int fd_;
    struct termios saved_{};
};

int attach_loop(cli::Client& client, const std::string& id, int host_in, int host_out);

}  // namespace terminal
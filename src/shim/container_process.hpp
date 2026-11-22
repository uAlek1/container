#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace shim {

struct SpawnRequest {
    std::string rootfs;
    std::vector<std::string> argv;
    bool tty = false;
    std::string log_path; 
};

struct SpawnResult {
    int32_t pid = 0;     
    int master_fd = -1;    
    int log_fd = -1;       
};

struct Error {
    int stage = 0;
    int errnum = 0;
    std::string message;
};

std::expected<SpawnResult, Error> spawn(const SpawnRequest& req);

int wait_for_exit(int32_t pid);

}  // namespace shim
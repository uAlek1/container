#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "container_process.hpp"
#include "service.hpp"

namespace {

struct Spec {
    std::string id;
    std::string rootfs;
    std::string socket_path;
    std::string log_path;
    bool tty = false;
    std::vector<std::string> argv;
};

void print_usage() {
    std::fprintf(stderr,
        "Usage: container-shim --id ID --rootfs DIR --socket PATH "
        "[--log PATH] [--tty] -- CMD [ARGS...]\n");
}

bool parse_spec(int argc, char** argv, Spec& out, std::string& err) {
    int i = 1;
    bool saw_dashdash = false;

    while (i < argc) {
        std::string a = argv[i];
        if (a == "--") {
            saw_dashdash = true;
            ++i;
            break;
        } else if (a == "--id") {
            if (++i >= argc) { err = "--id requires value"; return false; }
            out.id = argv[i];
        } else if (a == "--rootfs") {
            if (++i >= argc) { err = "--rootfs requires value"; return false; }
            out.rootfs = argv[i];
        } else if (a == "--socket") {
            if (++i >= argc) { err = "--socket requires value"; return false; }
            out.socket_path = argv[i];
        } else if (a == "--log") {
            if (++i >= argc) { err = "--log requires value"; return false; }
            out.log_path = argv[i];
        } else if (a == "--tty") {
            out.tty = true;
        } else if (a == "-h" || a == "--help") {
            print_usage();
            std::exit(0);
        } else {
            err = "unknown argument: " + a;
            return false;
        }
        ++i;
    }

    if (!saw_dashdash) { err = "missing '--' before command"; return false; }
    if (out.id.empty()) { err = "--id required"; return false; }
    if (out.rootfs.empty()) { err = "--rootfs required"; return false; }
    if (out.socket_path.empty()) { err = "--socket required"; return false; }
    if (i >= argc) { err = "missing command after '--'"; return false; }

    for (; i < argc; ++i) out.argv.emplace_back(argv[i]);

    if (!out.tty && out.log_path.empty()) {
        err = "tty=false requires --log";
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);

    Spec spec;
    std::string err;
    if (!parse_spec(argc, argv, spec, err)) {
        std::fprintf(stderr, "container-shim: %s\n", err.c_str());
        print_usage();
        return 2;
    }

    // id_t pid = ::fork();
    // if (pid < 0) {
    //     perror("fork");
    //     return 1;
    // }
    // if (pid > 0) {
    //     ::_exit(0);
    // }

    if (::setsid() < 0 && errno != EPERM) {
        std::fprintf(stderr, "container-shim: setsid: %s\n", std::strerror(errno));
        return 1;
    }

    shim::SpawnRequest req;
    req.rootfs = spec.rootfs;
    req.argv = spec.argv;
    req.tty = spec.tty;
    req.log_path = spec.log_path;

    auto spawn_result = shim::spawn(req);
    if (!spawn_result) {
        std::fprintf(stderr, "container-shim: spawn: %s (stage %d, errno %d)\n",
                     spawn_result.error().message.c_str(),
                     spawn_result.error().stage,
                     spawn_result.error().errnum);
        return 1;
    }

    shim::ShimService service(spec.id,
                              spawn_result->pid,
                              spawn_result->master_fd,
                              spec.socket_path);
    return service.run();
}
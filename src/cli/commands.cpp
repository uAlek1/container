#include "commands.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <csignal>

#include "runtime.pb.h"

namespace cli {

    namespace {

    std::string_view state_to_string(runtime::v1::ContainerState s) {
        switch (s) {
            case runtime::v1::CONTAINER_STATE_CREATED: return "CREATED";
            case runtime::v1::CONTAINER_STATE_RUNNING: return "RUNNING";
            case runtime::v1::CONTAINER_STATE_STOPPED: return "STOPPED";
            case runtime::v1::CONTAINER_STATE_EXITED:  return "EXITED";
            default:                                   return "UNKNOWN";
        }
    }

    std::string join_argv(const google::protobuf::RepeatedPtrField<std::string>& argv) {
        std::string out;
        for (const auto& a : argv) {
            if (!out.empty()) out += ' ';
            out += a;
        }
        return out;
    }

    bool check_status(const runtime::v1::Status& st, std::string_view cmd) {
        if (st.code() == runtime::v1::STATUS_OK) {
            return true;
        }
        std::fprintf(stderr, "%.*s: %s\n",
                    static_cast<int>(cmd.size()), cmd.data(),
                    st.message().empty() ? "request failed" : st.message().c_str());
        return false;
    }

    volatile sig_atomic_t g_logs_interrupted = 0;
    extern "C" void logs_sigint_handler(int) {
        g_logs_interrupted = 1;
    }

    bool write_all_fd(int fd, const char* data, size_t n) {
        size_t total = 0;
        while (total < n) {
            ssize_t w = ::write(fd, data + total, n - total);
            if (w < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            total += static_cast<size_t>(w);
        }
        return true;
    }

    }  // namespace

int cmd_list(Client& client, std::span<const std::string_view> args) {
    bool include_stopped = false;
    for (auto a : args) {
        if (a == "--all") {
            include_stopped = true;
        } else if (a == "-h" || a == "--help") {
            std::printf("Usage: your-cli list [--all]\n");
            return kExitOk;
        } else {
            std::fprintf(stderr, "list: unknown argument '%.*s'\n",
                         static_cast<int>(a.size()), a.data());
            return kExitUsage;
        }
    }

    runtime::v1::ClientMessage req;
    req.mutable_list()->set_include_stopped(include_stopped);

    try {
        client.send(req);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "list: %s\n", e.what());
        return kExitError;
    }

    runtime::v1::ServerMessage resp;
    try {
        if (!client.recv(resp)) {
            std::fprintf(stderr, "list: daemon closed connection\n");
            return kExitError;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "list: %s\n", e.what());
        return kExitError;
    }

    if (!resp.has_list()) {
        std::fprintf(stderr, "list: unexpected response type\n");
        return kExitError;
    }

    const auto& list = resp.list();
    if (!check_status(list.status(), "list")) {
        return kExitError;
    }

    if (list.containers().empty()) {
        return kExitOk;
    }

    std::printf("%-16s %-10s %-8s %-12s %s\n",
                "ID", "STATE", "PID", "CREATED", "COMMAND");
    for (const auto& c : list.containers()) {
        std::string cmd = join_argv(c.argv());
        std::printf("%-16s %-10.*s %-8d %-12ld %s\n",
                    c.id().c_str(),
                    static_cast<int>(state_to_string(c.state()).size()),
                    state_to_string(c.state()).data(),
                    c.pid(),
                    static_cast<long>(c.created_at()),
                    cmd.c_str());
    }
    return kExitOk;
}

int cmd_run(Client& client, std::span<const std::string_view> args) {
    bool detached = false;
    bool force_tty = false;
    std::vector<std::pair<std::string, std::string>> env;
    std::string cwd;
    std::vector<std::string> positional;

    for (size_t i = 0; i < args.size(); ++i) {
        auto a = args[i];
        if (a == "-d" || a == "--detach") {
            detached = true;
        } else if (a == "--tty" || a == "-t") {
            force_tty = true;
        } else if (a == "--cwd") {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "run: --cwd requires an argument\n");
                return kExitUsage;
            }
            cwd = std::string(args[++i]);
        } else if (a == "-h" || a == "--help") {
            std::printf("Usage: your-cli run [-d] [--tty] [-e K=V]... [--cwd DIR] <rootfs> [<cmd> [args...]]\n");
            std::printf("  -d, --detach   запустить в фоне, напечатать id и выйти\n");
            std::printf("  -t, --tty      выделить PTY (по умолчанию — без -d)\n");
            std::printf("  -e, --env K=V  переменная окружения (можно повторять)\n");
            std::printf("  --cwd DIR      рабочая директория внутри контейнера\n");
            std::printf("\nЕсли <cmd> не задан, используется /bin/sh.\n");
            return kExitOk;
        } else if (!a.empty() && a.front() == '-' && a.size() > 1) {
            if (a == "--") {
                for (size_t j = i + 1; j < args.size(); ++j) {
                    positional.emplace_back(args[j]);
                }
                break;
            }
            std::fprintf(stderr, "run: unknown argument '%.*s'\n",
                         static_cast<int>(a.size()), a.data());
            return kExitUsage;
        } else {
            positional.emplace_back(a);
        }
    }

    if (positional.empty()) {
        std::fprintf(stderr, "run: missing <rootfs>\n");
        return kExitUsage;
    }

    std::string rootfs = positional[0];
    std::vector<std::string> argv(positional.begin() + 1, positional.end());
    if (argv.empty()) {
        argv.emplace_back("/bin/sh");
    }

    bool tty = force_tty || !detached;
    if (!detached && !::isatty(STDIN_FILENO)) {
        std::fprintf(stderr, "run: interactive mode requires a TTY on stdin "
                             "(use -d for detached mode)\n");
        return kExitError;
    }

    runtime::v1::ClientMessage req;
    auto* cr = req.mutable_create();
    cr->set_rootfs(rootfs);
    for (const auto& a : argv) {
        cr->add_argv(a);
    }
    cr->set_tty(tty);
    cr->set_detached(detached);
    if (!cwd.empty()) {
        cr->set_cwd(cwd);
    }

    try {
        client.send(req);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "run: %s\n", e.what());
        return kExitError;
    }

    runtime::v1::ServerMessage resp;
    try {
        if (!client.recv(resp)) {
            std::fprintf(stderr, "run: daemon closed connection\n");
            return kExitError;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "run: %s\n", e.what());
        return kExitError;
    }

    if (!resp.has_create()) {
        std::fprintf(stderr, "run: unexpected response type\n");
        return kExitError;
    }

    const auto& create = resp.create();
    if (!check_status(create.status(), "run")) {
        return kExitError;
    }

    if (create.id().empty()) {
        std::fprintf(stderr, "run: daemon returned empty container id\n");
        return kExitError;
    }

    if (detached) {
        std::printf("%s\n", create.id().c_str());
        return kExitOk;
    }

    try {
        terminal::RawMode raw(STDIN_FILENO);
        return terminal::attach_loop(client, STDIN_FILENO, STDOUT_FILENO);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "run: %s\n", e.what());
        return kExitError;
    }
}

int cmd_logs(Client& client, std::span<const std::string_view> args) {
    std::string id;
    bool follow = false;
    int64_t tail_lines = 0;

    for (size_t i = 0; i < args.size(); ++i) {
        auto a = args[i];
        if (a == "-f" || a == "--follow") {
            follow = true;
        } else if (a == "--tail") {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "logs: --tail requires an argument\n");
                return kExitUsage;
            }
            std::string_view v = args[++i];
            int64_t parsed = 0;
            auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), parsed);
            if (ec != std::errc{} || ptr != v.data() + v.size() || parsed < 0) {
                std::fprintf(stderr, "logs: invalid --tail value '%.*s'\n",
                             static_cast<int>(v.size()), v.data());
                return kExitUsage;
            }
            tail_lines = parsed;
        } else if (a == "-h" || a == "--help") {
            std::printf("Usage: your-cli logs [-f] [--tail N] <id>\n");
            std::printf("  -f, --follow   следить за логами в реальном времени\n");
            std::printf("  --tail N       показать последние N строк (0 — всё)\n");
            return kExitOk;
        } else if (!a.empty() && a.front() == '-') {
            std::fprintf(stderr, "logs: unknown argument '%.*s'\n",
                         static_cast<int>(a.size()), a.data());
            return kExitUsage;
        } else {
            id = std::string(a);
        }
    }

    if (id.empty()) {
        std::fprintf(stderr, "logs: missing container id\n");
        return kExitUsage;
    }

    struct sigaction old_sa{};
    bool installed = false;
    if (follow) {
        g_logs_interrupted = 0;
        struct sigaction sa{};
        sa.sa_handler = logs_sigint_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        if (sigaction(SIGINT, &sa, &old_sa) == 0) {
            installed = true;
        }
    }

    auto restore_sigint = [&]() {
        if (installed) {
            sigaction(SIGINT, &old_sa, nullptr);
        }
    };

    runtime::v1::ClientMessage req;
    req.mutable_logs()->set_id(id);
    req.mutable_logs()->set_follow(follow);
    req.mutable_logs()->set_tail_lines(tail_lines);

    try {
        client.send(req);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "logs: %s\n", e.what());
        restore_sigint();
        return kExitError;
    }

    int rc = kExitOk;
    while (true) {
        if (g_logs_interrupted) {
            rc = kExitOk;
            break;
        }

        runtime::v1::ServerMessage resp;
        try {
            if (!client.recv(resp)) {
                if (!follow) {
                    std::fprintf(stderr, "logs: daemon closed connection unexpectedly\n");
                    rc = kExitError;
                }
                break;
            }
        } catch (const std::exception& e) {
            if (g_logs_interrupted) {
                rc = kExitOk;
            } else {
                std::fprintf(stderr, "logs: %s\n", e.what());
                rc = kExitError;
            }
            break;
        }

        if (!resp.has_log()) {
            std::fprintf(stderr, "logs: unexpected response type\n");
            rc = kExitError;
            break;
        }

        const auto& chunk = resp.log();

        if (chunk.status().code() != runtime::v1::STATUS_OK) {
            check_status(chunk.status(), "logs");
            rc = kExitError;
            break;
        }

        if (!chunk.data().empty()) {
            int fd = chunk.is_stderr() ? STDERR_FILENO : STDOUT_FILENO;
            if (!write_all_fd(fd, chunk.data().data(), chunk.data().size())) {
                std::fprintf(stderr, "logs: write failed: %s\n", std::strerror(errno));
                rc = kExitError;
                break;
            }
        }

        if (chunk.eof()) {
            break;
        }
    }

    restore_sigint();
    return rc;
}

int cmd_attach(Client& client, std::span<const std::string_view> args) {
    std::string id;
    for (auto a : args) {
        if (a == "-h" || a == "--help") {
            std::printf("Usage: your-cli attach <id>\n");
            return kExitOk;
        } else if (!a.empty() && a.front() == '-') {
            std::fprintf(stderr, "attach: unknown argument '%.*s'\n",
                         static_cast<int>(a.size()), a.data());
            return kExitUsage;
        } else {
            if (!id.empty()) {
                std::fprintf(stderr, "attach: unexpected extra argument '%.*s'\n",
                             static_cast<int>(a.size()), a.data());
                return kExitUsage;
            }
            id = std::string(a);
        }
    }
    if (id.empty()) {
        std::fprintf(stderr, "attach: missing container id\n");
        return kExitUsage;
    }

    if (!::isatty(STDIN_FILENO)) {
        std::fprintf(stderr, "attach: attach requires a TTY on stdin\n");
        return kExitError;
    }

    try {
        terminal::RawMode raw(STDIN_FILENO);
        return terminal::attach_loop(client, STDIN_FILENO, STDOUT_FILENO);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "attach: %s\n", e.what());
        return kExitError;
    }
}

int cmd_kill(Client& client, std::span<const std::string_view> args) {
    std::string id;

    for (auto a : args) {
        if (a == "-h" || a == "--help") {
            std::printf("Usage: your-cli kill <id>\n");
            std::printf("  Отправляет SIGTERM контейнеру.\n");
            return kExitOk;
        } else {
            id = std::string(a);
        }
    }

    if (id.empty()) {
        std::fprintf(stderr, "kill: missing container id\n");
        return kExitUsage;
    }

    runtime::v1::ClientMessage req;
    req.mutable_kill()->set_id(id);

    try {
        client.send(req);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "kill: %s\n", e.what());
        return kExitError;
    }

    runtime::v1::ServerMessage resp;
    try {
        if (!client.recv(resp)) {
            std::fprintf(stderr, "kill: daemon closed connection\n");
            return kExitError;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "kill: %s\n", e.what());
        return kExitError;
    }

    if (!resp.has_kill()) {
        std::fprintf(stderr, "kill: unexpected response type\n");
        return kExitError;
    }

    if (!check_status(resp.kill().status(), "kill")) {
        return kExitError;
    }

    return kExitOk;
}


}  // namespace cli
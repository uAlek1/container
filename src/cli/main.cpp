#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "commands.hpp"
#include "path.hpp"

namespace cli {

constexpr std::string_view kDefaultSocket = "/run/container/daemon.sock";

struct CommandSpec {
    std::string_view name;
    std::string_view help;
};

constexpr CommandSpec kCommands[] = {
    {"run",    "Запустить контейнер. Без -d — интерактивно с PTY, с -d — в фоне."},
    {"list",   "Показать список контейнеров."},
    {"logs",   "Показать логи контейнера. -f — следить в реальном времени."},
    {"attach", "Подключиться к терминалу запущенного контейнера."},
    {"kill",   "Отправить сигнал контейнеру. По умолчанию SIGTERM."},
};

void print_usage(std::FILE* out) {
    std::fprintf(out, "Usage: your-cli <command> [args...]\n\nCommands:\n");
    for (const auto& c : kCommands) {
        std::fprintf(out, "  %-8.*s %.*s\n",
                     static_cast<int>(c.name.size()), c.name.data(),
                     static_cast<int>(c.help.size()), c.help.data());
    }
    std::fprintf(out, "\nRun 'your-cli <command> --help' for details.\n");
}

int dispatch(Client& client, std::string_view name,
             std::span<const std::string_view> args) {
    if (name == "run")    return cmd_run(client, args);
    if (name == "list")   return cmd_list(client, args);
    if (name == "logs")   return cmd_logs(client, args);
    if (name == "attach") return cmd_attach(client, args);
    if (name == "kill")   return cmd_kill(client, args);

    std::fprintf(stderr, "your-cli: unknown command '%.*s'\n",
                 static_cast<int>(name.size()), name.data());
    print_usage(stderr);
    return kExitUsage;
}

}  // namespace cli

int main(int argc, char** argv) {

    if (argc < 2) {
        cli::print_usage(stderr);
        return cli::kExitUsage;
    }

    std::string_view name = argv[1];

    if (name == "--help" || name == "-h" || name == "help") {
        cli::print_usage(stdout);
        return cli::kExitOk;
    }

    std::vector<std::string_view> tail;
    tail.reserve(static_cast<size_t>(argc - 2));
    for (int i = 2; i < argc; ++i) {
        tail.emplace_back(argv[i]);
    }

    cli::Client client{common::daemon_socket_path().string()};
    try {
        client.connect();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "your-cli: %s\n", e.what());
        return cli::kExitError;
    }

    return cli::dispatch(client, name, tail);
}
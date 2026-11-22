#pragma once

#include <span>
#include <string_view>

#include "client.hpp"
#include "terminal.hpp"

namespace cli {

inline constexpr int kExitOk = 0;
inline constexpr int kExitError = 1;
inline constexpr int kExitUsage = 2;

int cmd_run(Client& client, std::span<const std::string_view> args);
int cmd_list(Client& client, std::span<const std::string_view> args);
int cmd_logs(Client& client, std::span<const std::string_view> args);
int cmd_attach(Client& client, std::span<const std::string_view> args);
int cmd_kill(Client& client, std::span<const std::string_view> args);

}  // namespace cli
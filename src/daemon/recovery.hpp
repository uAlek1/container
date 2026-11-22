#pragma once

#include "registry.hpp"
#include "shim_manager.hpp"

namespace cont_daemon {

int recover_containers(ContainerRegistry& registry, ShimManager& shim);

}  // namespace cont_daemon
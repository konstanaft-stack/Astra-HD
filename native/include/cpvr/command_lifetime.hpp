#pragma once
#include "gpu.hpp"
#include <atomic>
namespace cpvr {
// Records original-object destruction without retaining the original object.
// This retires replay possibility, NOT unfinished GPU executions/fences.
struct CommandLifetime {std::atomic<bool> destroyed{};};
std::shared_ptr<CommandLifetime> observe_command_lifetime(ID3D12GraphicsCommandList*) noexcept;
}

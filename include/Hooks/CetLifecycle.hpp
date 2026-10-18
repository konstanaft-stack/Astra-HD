#pragma once
#include "Render/CetLifecycleDispatch.hpp"
#include <cstdint>
namespace cvr::cet {
bool LifecycleRequested() noexcept;
bool ProducerHooksInstalled() noexcept;
bool ProducerBootstrapPending() noexcept;
cvr::gpu::CetLifecycleDispatch& Lifetime();
// Caller must already own producer/reset admission before using queue snapshots.
bool DrainGraphics(uint32_t timeoutMs) noexcept;
class ControlledResetScope {
public:
    ControlledResetScope() noexcept;
    ~ControlledResetScope();
    ControlledResetScope(const ControlledResetScope&)=delete;
    ControlledResetScope& operator=(const ControlledResetScope&)=delete;
};
}

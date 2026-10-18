#pragma once
#include "Render/ResizeQuiescence.hpp"
namespace cvr::framegen {
cvr::gpu::QueueDrainResult DrainNativeFgResizeQueuesChecked(uint32_t timeoutMs)noexcept;
}

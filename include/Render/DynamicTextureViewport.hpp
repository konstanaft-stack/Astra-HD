#pragma once
#include <cstdint>
namespace cvr::render {
// Reuses the pinned game resource API already used by the RTT driver.
// True means dispatch only; verify the consuming GPU resource extent.
bool RequestDynamicTextureViewport(void* resource,uint32_t width,uint32_t height)noexcept;
}

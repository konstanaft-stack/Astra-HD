#pragma once
#include <dxgi.h>
namespace cvr::menu_preview {
// Called only after an eligible real game Present. No marker environment => no-op.
void RenderDocAfterGamePresent(IDXGISwapChain*,HRESULT)noexcept;
}

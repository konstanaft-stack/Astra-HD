#pragma once
#include "Overlay/PpdNativeApply.hpp"
namespace cvr::hooks {
// Called under PPD's short ownership lock. Never waits for CPU/GPU here.
bool AdmitNativePpdWrite(const cvr::ppd::NativeApplyStatus&,void* settings) noexcept;
enum class AuthorSizeAdmission {Denied,Bootstrap,PendingOnly};
AuthorSizeAdmission AdmitAuthorSizeWrite(uintptr_t settings,uint32_t width,uint32_t height) noexcept;
}

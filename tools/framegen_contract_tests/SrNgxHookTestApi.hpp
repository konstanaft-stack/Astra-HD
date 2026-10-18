#pragma once
#include "Framegen/SrNativeApi.hpp"
#include <sl_core_types.h>
#include <cstddef>
namespace cvr::framegen::sr_test {
struct Module {uintptr_t base{};void* targets[4]{};};
struct Platform {
    bool (*sl)(Module&);
    bool (*native)(Module&);
    bool (*pin)(uintptr_t);
    int (*create)(void*,void*,void**);
    int (*queueEnable)(void*);
    int (*apply)();
    int (*disable)(void*);
    int (*remove)(void*);
};
void Reset(const Platform&); // Test build only, no actual hooks may be installed.
bool DriverStorePath(const wchar_t* path,const wchar_t* windows);
NVSDK_NGX_Result Create(ID3D12GraphicsCommandList*,NVSDK_NGX_Feature,NVSDK_NGX_Parameter*,NVSDK_NGX_Handle**);
NVSDK_NGX_Result Evaluate(ID3D12GraphicsCommandList*,const NVSDK_NGX_Handle*,const NVSDK_NGX_Parameter*,PFN_NVSDK_NGX_ProgressCallback);
NVSDK_NGX_Result Release(NVSDK_NGX_Handle*);
sl::Result Init(const sl::Preferences&,uint64_t);
sl::Result SetDevice(void*);
sl::Result Allocate(void*,sl::Feature,const sl::ViewportHandle&);
sl::Result Shutdown();
}

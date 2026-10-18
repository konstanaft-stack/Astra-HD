#pragma once
#include "Framegen/ColorRead.hpp"
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <cstdint>
#include <optional>

namespace cvr::framegen {
struct CaptureCompletion {
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    uint64_t fenceValue{};
    // CPU time immediately after scheduling this copy. This is neither the
    // pose prediction time nor measured GPU completion time.
    LARGE_INTEGER qpc{};
    int64_t qpcFrequency{};
    D3D12_RESOURCE_STATES colorState=CaptureColorReadState;
    static CaptureCompletion AfterSignal(ID3D12Fence* fence,uint64_t value,HRESULT result,LARGE_INTEGER qpc,int64_t frequency) {
        CaptureCompletion out;
        if(FAILED(result) || !fence || !value || value==UINT64_MAX)return out;
        out.fence=fence;out.fenceValue=value;out.qpc=qpc;out.qpcFrequency=frequency;return out;
    }
    // Scheduled producer completion, not proof that the GPU reached it.
    // A consumer must retain the author's color ReadLease and wait this fence.
    bool HasScheduledCopySource() const {
        return fence && fenceValue && fenceValue!=UINT64_MAX &&
            (colorState&D3D12_RESOURCE_STATE_COPY_SOURCE)!=0;
    }
    // Only the runtime can map QPC into XrTime. Caller must pass the function
    // obtained for an instance with the corresponding extension enabled.
    std::optional<XrTime> ToSubmissionXrTime(XrInstance instance,PFN_xrConvertWin32PerformanceCounterToTimeKHR convert) const {
        if(!HasScheduledCopySource() || !instance || !convert || qpc.QuadPart<=0 || qpcFrequency<=0)return {};
        XrTime result{};
        if(XR_FAILED(convert(instance,&qpc,&result)) || result<=0)return {};
        return result;
    }
};
}

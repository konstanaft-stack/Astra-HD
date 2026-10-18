#include "Framegen/FfxColorRead.hpp"
#include "CpuD3d.hpp"
#include <dxgi1_4.h>
#include <FidelityFX/host/ffx_interface.h>
#include <FidelityFX/host/backends/dx12/d3dx12.h>
#include <iostream>
#include <stdexcept>
#include <mutex>
#include <memory>
static void Check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
#undef FFX_ASSERT
#undef FFX_ASSERT_MESSAGE
#define FFX_ASSERT(value) Check(bool(value),"pinned SDK assertion")
#define FFX_ASSERT_MESSAGE(value,message) Check(bool(value),message)
#include "PinnedFfxStates.inl"
int main() {
    try {
        using namespace cvr::framegen;
        Microsoft::WRL::ComPtr<CpuDevice> device;device.Attach(new CpuDevice);
        Microsoft::WRL::ComPtr<CpuResource> color;color.Attach(new CpuResource(device.Get()));
        Microsoft::WRL::ComPtr<CpuGraphicsList> list;list.Attach(new CpuGraphicsList(device.Get()));
        auto context=std::make_unique<BackendContext_DX12>();
        auto resources=std::make_unique<BackendContext_DX12::Resource[]>(FFX_MAX_RESOURCE_COUNT);
        BackendContext_DX12::EffectContext effect{};
        context->pResources=resources.get();context->pEffectContexts=&effect;
        const auto index=FFX_MAX_RESOURCE_COUNT-1;
        resources[index].resourcePtr=color.Get();
        resources[index].initialState=resources[index].currentState=CaptureColorFfxState;
        effect.nextDynamicResource=index-1;
        FfxResourceInternal source{index};
        // Actual pinned FI schedules source SRV compute jobs, then the source
        // COPY to its previous-interpolation texture (FI lines 1019/1200).
        // Its DX12 backend requests these states at lines 3166/3337.
        addBarrier(context.get(),&source,FFX_RESOURCE_STATE_COMPUTE_READ);
        addBarrier(context.get(),&source,FFX_RESOURCE_STATE_COPY_SRC);
        FfxInterface backend{};backend.scratchBuffer=context.get();
        Check(UnregisterResourcesDX12(&backend,reinterpret_cast<FfxCommandList>(list.Get()),0)==FFX_OK,
            "actual SDK unregister failed");
        Check(list->barriers.empty(),"pinned FFX narrowed the shared currentBackBuffer state");
        Check(ffxGetDX12StateFromResourceState(CaptureColorFfxState)==CaptureColorReadState,
            "FFX source declaration does not match the actual D3D12 initial state");
        Check(ffxGetDX12StateFromResourceState(resources[index].currentState)==CaptureColorReadState,
            "post-dispatch/unregister source state changed");
        Check(list->unexpected==0,"state fixture executed unrelated GPU commands");
        std::cout<<"PASS pinned FFX compute/copy/unregister preserves source state\n";return 0;
    } catch(const std::exception& error) {std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
}

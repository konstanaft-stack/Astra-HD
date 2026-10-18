#include "Framegen/ColorRead.hpp"
#include "CpuD3d.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
static void Check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
struct AllocationProbe {
    CpuDevice* identity;std::vector<D3D12_RESOURCE_STATES> states;
    HRESULT CreateCommittedResource(const D3D12_HEAP_PROPERTIES* heap,D3D12_HEAP_FLAGS flags,
        const D3D12_RESOURCE_DESC* description,D3D12_RESOURCE_STATES state,const D3D12_CLEAR_VALUE* clear,REFIID id,void** out) {
        Check(heap->Type==D3D12_HEAP_TYPE_DEFAULT && flags==D3D12_HEAP_FLAG_NONE && !clear && id==__uuidof(ID3D12Resource),
            "MAIN allocation contract changed");
        auto* resource=new CpuResource(identity);resource->descriptor=*description;*out=resource;states.push_back(state);return S_OK;
    }
};
int main() {
    try {
        using namespace cvr::framegen;
        Microsoft::WRL::ComPtr<CpuDevice> device;device.Attach(new CpuDevice);
        AllocationProbe allocator{device.Get()};
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC description{};description.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.DepthOrArraySize=description.MipLevels=description.SampleDesc.Count=1;description.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        for(unsigned size:{16u,32u}) {
            std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,3> slots;allocator.states.clear();
            description.Width=description.Height=size;
            for(auto& slot:slots)Check(SUCCEEDED(CreateMainCaptureColor(&allocator,heap,description,slot.GetAddressOf())),"MAIN creation failed");
            for(unsigned frame=0;frame<9;++frame) {
                const unsigned slot=(frame+1)%3;auto& state=allocator.states[slot];
                const auto apply=[&](const D3D12_RESOURCE_BARRIER& barrier) {
                    Check(barrier.Transition.pResource==slots[slot].Get(),"producer barrier addressed another slot");
                    Check(barrier.Transition.StateBefore==state,"MAIN first-use/reuse/resize before-state mismatched allocation");
                    state=barrier.Transition.StateAfter;
                };
                apply(MainCaptureWriteBarrier(slots[slot].Get()));
                Check(state==D3D12_RESOURCE_STATE_COPY_DEST,"reserved slot was not writable for capture");
                apply(MainCaptureReadyBarrier(slots[slot].Get()));
                Check(state==CaptureColorReadState && slots[slot]->GetDesc().Width==size,"published slot state/resize mismatch");
            }
        }
        std::cout<<"PASS MAIN three-slot first-use, cycling and resize\n";return 0;
    } catch(const std::exception& error) {std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
}

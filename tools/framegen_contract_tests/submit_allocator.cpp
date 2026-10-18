#include "Runtimes/SubmitFenceRetirement.hpp"
#include <array>
#include <stdexcept>
#include <iostream>
struct FakeFence {
    uint64_t completed=8,requested{};bool finishOnWait{};
    uint64_t GetCompletedValue(){return completed;}
    HRESULT SetEventOnCompletion(uint64_t at,HANDLE){requested=at;return S_OK;}
};
static DWORD FakeWait(HANDLE handle,DWORD){auto* f=static_cast<FakeFence*>(handle);if(f->finishOnWait){f->completed=f->requested;return WAIT_OBJECT_0;}return WAIT_TIMEOUT;}
struct FakeAllocator {unsigned resets{};HRESULT Reset(){++resets;return S_OK;}};
struct FakeList {HRESULT Reset(FakeAllocator*,void*){return S_OK;}};
#define ID3D12CommandAllocator FakeAllocator
#define ID3D12GraphicsCommandList FakeList
#define WaitForSingleObject FakeWait
struct Owner {
    FakeFence fence;FakeFence* m_fence=&fence;HANDLE m_fenceEvent=&fence;
    uint64_t m_fenceValue=10;uint32_t m_cmdAllocatorIndex=2;
    std::array<uint64_t,3> m_submitAllocatorFences{10,0,0};
    FakeAllocator allocators[3];FakeList lists[3];
    FakeAllocator* m_cmdAllocators[3]{&allocators[0],&allocators[1],&allocators[2]};
    FakeList* m_cmdLists[3]{&lists[0],&lists[1],&lists[2]};
    void* monoSource=this;bool monoHasView[2]{true,true};unsigned recorded{};
    void Step(){
#include "allocator_step.inc"
    }
};
static void Check(bool v,const char* m){if(!v)throw std::runtime_error(m);}
int main(){try{
    {Owner o;o.Step();Check(o.allocators[0].resets==0,"pending slot reset after two skipped submissions; last-global minus two is not this slot's fence");}
    {Owner o;o.fence.completed=10;o.Step();Check(o.recorded==1,"completed exact slot cannot be reused");}
    {Owner o;o.fence.completed=5;o.Step();Check(o.recorded==0&&o.fence.requested==10,"timeout bypassed the exact allocator fence");}
    {Owner o;o.fence.completed=5;o.fence.finishOnWait=true;o.Step();Check(o.recorded==1&&o.fence.completed==10,"wait did not target this allocator's submission");}
    {Owner o;o.fence.completed=UINT64_MAX;o.Step();Check(o.recorded==0,"removed device treated as completed GPU work");}
    {Owner o;o.m_submitAllocatorFences[0]=UINT64_MAX;o.Step();Check(o.recorded==0,"unknown Signal completion permitted allocator reuse");}
    {Owner o;o.m_submitAllocatorFences[0]=0;o.Step();Check(o.recorded==1,"fresh unused allocator rejected");}
    std::cout<<"PASS exact production allocator wait/reset, skipped cycles, timeout, completion and removed-device cases=7\n";return 0;
}catch(const std::exception& e){std::cout<<"FAIL "<<e.what()<<'\n';return 1;}}

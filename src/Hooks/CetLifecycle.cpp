#include "Hooks/CetLifecycle.hpp"
#include "Hooks/Hook.hpp"
#include "Framegen/NativeFgOutput.hpp"
#include "Framegen/ResizeQueueDrain.hpp"
#include "Utils/MemorySafe.hpp"
#include <Windows.h>
#include <MinHook.h>
#include <atomic>
#include <chrono>
#include <cstring>

extern void Log(const char*,...);
extern "C" {
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_CetProducerPresents{0};
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_CetProducerSkipped{0};
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_CetUncontrolledResets{0};
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_CetDispatchMismatch{0};
__declspec(dllexport) std::atomic<uint32_t> CyberpunkVR_CetDrainInitialized{255};
__declspec(dllexport) std::atomic<uint32_t> CyberpunkVR_CetDrainNoReferences{0};
}

namespace cvr::cet {
namespace {
using namespace std::chrono_literals;
using Present=void*(*)(int32_t*,uint8_t,UINT);
using Resize=void*(*)(uint32_t,uint32_t,uint32_t,uint8_t,int*);
using RendererShutdown=void*(*)(uint64_t,uint64_t,uint64_t,uint64_t);
using Reader=void(*)(void*);
using TextureLoad=void*(*)(void*,const void*);
using Shutdown=void(*)();
using Delete=void(*)(void*,void*);
using DllEntry=BOOL(WINAPI*)(HINSTANCE,DWORD,LPVOID);
using WindowDispatch=LRESULT(WINAPI*)(WNDPROC,HWND,UINT,WPARAM,LPARAM);
Present originalPresent{},savedPresent{};
Resize originalResize{};
RendererShutdown originalRendererShutdown{};
Reader originalPrepare{},originalFonts{};
TextureLoad originalTexture{};
WNDPROC originalWindow{};
WindowDispatch savedWindowDispatch{};
Shutdown originalShutdown{};
Delete originalDelete{};
DllEntry originalDllEntry{};
uintptr_t moduleBase{},cetObject{},d3dObject{},windowObject{};
void**presentSlot{},**windowDispatchSlot{};
std::atomic<WNDPROC> savedWindowProc{};
std::atomic<bool> installed{},dispatchRetired{};
std::atomic<bool> installAttempted{};
std::atomic<void*> heldObject{};
thread_local unsigned controlledResetDepth{};
thread_local bool processDetach{},processTermination{};
struct WindowFrame {gpu::CetLifecycleDispatch::FrontendLease lease;WindowFrame*previous;};
thread_local WindowFrame*windowFrame{};

bool Executable(const void*pointer){
    MEMORY_BASIC_INFORMATION info{};
    if(!pointer||!VirtualQuery(pointer,&info,sizeof(info))||info.State!=MEM_COMMIT||
       (info.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
    return (info.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))!=0;
}
bool SwapOwnedSlot(void**slot,void*expected,void*replacement){
    DWORD protection{};if(!slot||!VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&protection))return false;
    const auto previous=InterlockedCompareExchangePointer(reinterpret_cast<void*volatile*>(slot),replacement,expected);
    DWORD ignored{};VirtualProtect(slot,sizeof(void*),protection,&ignored);
    return previous==expected;
}
WNDPROC WindowTarget(){
    // Window and its dispatch slot are not reset by D3D12::ResetState. Full
    // invocation ownership, not the shorter GPU-reader lease, protects this read.
    uintptr_t liveWindow{},proc{};
    if(ReadPtrSafe(moduleBase+0x6dd958,&liveWindow)&&liveWindow==windowObject&&
       ReadPtrSafe(windowObject+0x10,&proc)&&Executable(reinterpret_cast<void*>(proc)))
        savedWindowProc.store(reinterpret_cast<WNDPROC>(proc),std::memory_order_release);
    return savedWindowProc.load(std::memory_order_acquire);
}
bool NoGraphicsReferences(){
    for(const auto offset:{0x60u,0x68u,0x70u,0x78u,0x80u}){
        uintptr_t value{};if(!ReadPtrSafe(d3dObject+offset,&value)||value)return false;
    }
    return true;
}
void ObserveQueue(){
    uintptr_t queue{};
    if(ReadPtrSafe(d3dObject+0x80,&queue)&&queue)
        framegen::ObserveNativeFgResizeQueue(reinterpret_cast<ID3D12CommandQueue*>(queue));
}

void* PresentDispatch(int32_t*device,uint8_t sync,UINT interval){
    ObserveQueue();
    if(!Lifetime().BeforeOriginalPresent()){
        ++CyberpunkVR_CetDispatchMismatch;
        Log("[CET-LIFECYCLE] original Present without admitted frontend; coverage not established\n");
    }
    return savedPresent(device,sync,interval);
}
void* PresentFrontend(int32_t*device,uint8_t sync,UINT interval){
    ++CyberpunkVR_CetProducerPresents;
    auto result=Lifetime().Present([&]{return originalPresent(device,sync,interval);},
                                  [&]{return savedPresent(device,sync,interval);});
    if(!result.admitted)++CyberpunkVR_CetProducerSkipped;
    if(result.admitted&&!result.forwarded){++CyberpunkVR_CetDispatchMismatch;
        Log("[CET-LIFECYCLE] Present dispatch slot no longer matches; coverage not established\n");}
    return result.value;
}
void Prepare(void*self){auto lease=Lifetime().TryEnterReader();if(lease)originalPrepare(self);}
void Fonts(void*self){auto lease=Lifetime().TryEnterReader();if(lease)originalFonts(self);}
void* Texture(void*out,const void*path){
    auto lease=Lifetime().TryEnterReader();
    if(lease)return originalTexture(out,path);
    // Verified MSVC x64 hidden shared_ptr result: pointer + control block.
    // This matches Texture::Load's native empty-result failure behavior.
    std::memset(out,0,2*sizeof(void*));return out;
}
LRESULT WINAPI WindowOriginal(WNDPROC previous,HWND hwnd,UINT message,WPARAM wp,LPARAM lp){
    if(windowFrame)windowFrame->lease.ReleaseProducer();
    return savedWindowDispatch(previous,hwnd,message,wp,lp);
}
LRESULT CALLBACK Window(HWND hwnd,UINT message,WPARAM wp,LPARAM lp){
    auto lease=Lifetime().TryEnterFrontend();
    if(!lease.HasInvocation())return DefWindowProcW(hwnd,message,wp,lp);
    const auto target=WindowTarget();
    if(!lease)return target?savedWindowDispatch(target,hwnd,message,wp,lp):DefWindowProcW(hwnd,message,wp,lp);
    WindowFrame frame{std::move(lease),windowFrame};
    struct Restore{WindowFrame*previous;~Restore(){windowFrame=previous;}}restore{windowFrame};
    windowFrame=&frame;
    return originalWindow(hwnd,message,wp,lp);
}
void NoteReset(const char*kind){
    if(!controlledResetDepth){const auto n=++CyberpunkVR_CetUncontrolledResets;
        if(n<=8||n%60==0)Log("[CET-LIFECYCLE] uncontrolled %s count=%llu; outside admitted native apply\n",kind,n);}
}
void* ResizeFrontend(uint32_t w,uint32_t h,uint32_t flags,uint8_t mode,int*device){
    NoteReset("resize");return originalResize(w,h,flags,mode,device);
}
void* RendererStop(uint64_t a,uint64_t b,uint64_t c,uint64_t d){
    NoteReset("renderer shutdown");return originalRendererShutdown(a,b,c,d);
}

void ShutdownFrontend(){
    // CET shutdown is called from DllMain and removes its MinHook trampolines
    // BEFORE nulling the singleton. Never wait for other threads or GPU there.
    const auto stopped=Lifetime().StopFor(0ms,[]{return NoGraphicsReferences();});
    if(stopped!=gpu::ProducerResetGate::StopResult::Stopped){
        if(processDetach&&processTermination){heldObject.store(reinterpret_cast<void*>(cetObject));
            Log("[CET-LIFECYCLE] process-exit hold: readers/resources not retired; retain exact CET object until OS reclamation\n");return;}
        Log("[CET-LIFECYCLE] unsupported nonterminal shutdown with active readers/resources; teardown suppressed\n");
        heldObject.store(reinterpret_cast<void*>(cetObject));return;
    }
    SwapOwnedSlot(presentSlot,reinterpret_cast<void*>(&PresentDispatch),reinterpret_cast<void*>(savedPresent));
    SwapOwnedSlot(windowDispatchSlot,reinterpret_cast<void*>(&WindowOriginal),reinterpret_cast<void*>(savedWindowDispatch));
    dispatchRetired.store(true,std::memory_order_release);
    originalShutdown();
}
void DeleteObject(void*a,void*object){
    // The CRT has a second singleton-deletion path even if Shutdown was held.
    if(object&&heldObject.load(std::memory_order_acquire)==object)return;
    originalDelete(a,object);
}
BOOL WINAPI Entry(HINSTANCE instance,DWORD reason,LPVOID reserved){
    const bool oldDetach=processDetach,oldTermination=processTermination;
    processDetach=reason==DLL_PROCESS_DETACH;processTermination=processDetach&&reserved!=nullptr;
    const auto result=originalDllEntry(instance,reason,reserved);
    processDetach=oldDetach;processTermination=oldTermination;return result;
}

bool Install(){
    installAttempted.store(true,std::memory_order_release);
    const auto module=GetModuleHandleW(L"cyber_engine_tweaks.asi");if(!module){Log("[CET-LIFECYCLE] CET unavailable at PreDevice; no late installation permitted\n");return false;}
    moduleBase=reinterpret_cast<uintptr_t>(module);
    const auto*dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(moduleBase);
    const auto*nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(moduleBase+dos->e_lfanew);
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE||nt->Signature!=IMAGE_NT_SIGNATURE||nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64||
       nt->FileHeader.TimeDateStamp!=0x68d9142a||nt->OptionalHeader.SizeOfImage!=0x730000)return false;
    if(!ReadPtrSafe(moduleBase+0x6dd8f0,&cetObject)||!cetObject)return false;
    d3dObject=cetObject+0x23e8;windowObject=cetObject+0x23a0;
    uint8_t initialized{};
    if(!ReadU8Safe(d3dObject+0x38,&initialized)||initialized||!NoGraphicsReferences()){
        Log("[CET-LIFECYCLE] renderer already initialized; refuse mid-flight installation\n");return false;}
    uintptr_t present{},dispatch{};
    presentSlot=reinterpret_cast<void**>(d3dObject+0x20);windowDispatchSlot=reinterpret_cast<void**>(moduleBase+0x5b49a0);
    if(!ReadPtrSafe(reinterpret_cast<uintptr_t>(presentSlot),&present)||!Executable(reinterpret_cast<void*>(present))||
       !ReadPtrSafe(reinterpret_cast<uintptr_t>(windowDispatchSlot),&dispatch)||!Executable(reinterpret_cast<void*>(dispatch)))return false;
    savedPresent=reinterpret_cast<Present>(present);savedWindowDispatch=reinterpret_cast<WindowDispatch>(dispatch);
    struct Hook{uintptr_t rva;const char*bytes;void*detour;void**original;};
    const Hook hooks[]{
        {0x2ba80,"\x40\x53\x55\x56\x57\x41\x54\x41\x56\x41\x57\x48\x81\xec\xb0\x01",reinterpret_cast<void*>(&PresentFrontend),reinterpret_cast<void**>(&originalPresent)},
        {0x2bd10,"\x4c\x8b\xdc\x49\x89\x5b\x10\x49\x89\x6b\x18\x56\x57\x41\x56\x48",reinterpret_cast<void*>(&ResizeFrontend),reinterpret_cast<void**>(&originalResize)},
        {0x2be10,"\x48\x89\x5c\x24\x08\x48\x89\x6c\x24\x10\x48\x89\x74\x24\x18\x48",reinterpret_cast<void*>(&RendererStop),reinterpret_cast<void**>(&originalRendererShutdown)},
        {0xccd0,"\x48\x89\x5c\x24\x18\x55\x56\x57\x41\x54\x41\x55\x41\x56\x41\x57",reinterpret_cast<void*>(&Prepare),reinterpret_cast<void**>(&originalPrepare)},
        {0xb2f0,"\x48\x89\x5c\x24\x10\x48\x89\x74\x24\x18\x48\x89\x7c\x24\x20\x55",reinterpret_cast<void*>(&Fonts),reinterpret_cast<void**>(&originalFonts)},
        {0x30fe00,"\x40\x53\x55\x56\x57\x41\x54\x41\x55\x41\x56\x41\x57\x48\x83\xec",reinterpret_cast<void*>(&Window),reinterpret_cast<void**>(&originalWindow)},
        {0x2fbe20,"\x48\x89\x5c\x24\x18\x55\x56\x57\x41\x54\x41\x55\x41\x56\x41\x57",reinterpret_cast<void*>(&Texture),reinterpret_cast<void**>(&originalTexture)},
        {0x2c640,"\x48\x89\x5c\x24\x10\x48\x89\x6c\x24\x18\x56\x57\x41\x54\x41\x56",reinterpret_cast<void*>(&ShutdownFrontend),reinterpret_cast<void**>(&originalShutdown)},
        {0x6630,"\x48\x85\xd2\x0f\x84\xe0\x00\x00\x00\x57\x48\x83\xec\x20\x48\x8d",reinterpret_cast<void*>(&DeleteObject),reinterpret_cast<void**>(&originalDelete)},
        {0x2c950,"\x48\x83\xec\x28\x85\xd2\x74\x14\x83\xfa\x01\x75\x14\xe8\xae\xf7",reinterpret_cast<void*>(&Entry),reinterpret_cast<void**>(&originalDllEntry)}
    };
    for(const auto&h:hooks)if(std::memcmp(reinterpret_cast<void*>(moduleBase+h.rva),h.bytes,16))return false;
    HMODULE pin{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(moduleBase),&pin))return false;
    size_t made=0;for(const auto&h:hooks){if(MH_CreateHook(reinterpret_cast<void*>(moduleBase+h.rva),h.detour,h.original)!=MH_OK)break;++made;}
    bool ok=made==std::size(hooks);
    if(ok)ok=SwapOwnedSlot(presentSlot,reinterpret_cast<void*>(savedPresent),reinterpret_cast<void*>(&PresentDispatch));
    if(ok)ok=SwapOwnedSlot(windowDispatchSlot,reinterpret_cast<void*>(savedWindowDispatch),reinterpret_cast<void*>(&WindowOriginal));
    if(ok)for(const auto&h:hooks)if(MH_QueueEnableHook(reinterpret_cast<void*>(moduleBase+h.rva))!=MH_OK){ok=false;break;}
    if(ok)ok=MH_ApplyQueued()==MH_OK;
    if(!ok){
        SwapOwnedSlot(presentSlot,reinterpret_cast<void*>(&PresentDispatch),reinterpret_cast<void*>(savedPresent));
        SwapOwnedSlot(windowDispatchSlot,reinterpret_cast<void*>(&WindowOriginal),reinterpret_cast<void*>(savedWindowDispatch));
        for(size_t i=0;i<made;++i){MH_DisableHook(reinterpret_cast<void*>(moduleBase+hooks[i].rva));MH_RemoveHook(reinterpret_cast<void*>(moduleBase+hooks[i].rva));}
        return false;
    }
    installed.store(true,std::memory_order_release);
    Log("[CET-LIFECYCLE] pristine PreDevice producer hooks installed\n");return true;
}
CVR_HOOK_IF("CetLifecycle",cvr::hooks::Stage::PreDevice,90,Install,LifecycleRequested);
}

bool LifecycleRequested()noexcept{static const bool requested=[] {char value[8]{};return !(GetEnvironmentVariableA("CPVR_CET_LIFECYCLE",value,sizeof(value))==1&&value[0]=='0');}();return requested;}
bool ProducerHooksInstalled()noexcept{return installed.load(std::memory_order_acquire)&&!dispatchRetired.load(std::memory_order_acquire)&&CyberpunkVR_CetDispatchMismatch.load(std::memory_order_acquire)==0;}
bool ProducerBootstrapPending()noexcept{return !installAttempted.load(std::memory_order_acquire);}
gpu::CetLifecycleDispatch& Lifetime(){static auto* state=new gpu::CetLifecycleDispatch;return *state;}
bool DrainGraphics(uint32_t timeoutMs)noexcept{
    ObserveQueue();uint8_t initialized=255;
    const bool initializedKnown=d3dObject&&ReadU8Safe(d3dObject+0x38,&initialized);
    const bool noReferences=NoGraphicsReferences();
    CyberpunkVR_CetDrainInitialized.store(initialized);
    CyberpunkVR_CetDrainNoReferences.store(noReferences);
    const auto queues=framegen::DrainNativeFgResizeQueuesChecked(timeoutMs);
    // Called with producer admission closed. Before any queued graphics work,
    // an explicitly uninitialized, reference-free CET renderer has nothing to
    // drain. All observed queues still fence-drain; failures never use this path.
    return gpu::ResizeQuiescent(queues,initializedKnown,initialized,noReferences);
}
ControlledResetScope::ControlledResetScope()noexcept{++controlledResetDepth;}
ControlledResetScope::~ControlledResetScope(){--controlledResetDepth;}
}

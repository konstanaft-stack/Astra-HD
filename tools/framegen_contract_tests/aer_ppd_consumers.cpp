#include <Windows.h>
#include "Overlay/PpdNativeApply.hpp"
#include <cstdint>
#include <iostream>
#include <stdexcept>
using namespace cvr::ppd;
namespace productionConsumers {
using DXGI_FORMAT=int;
struct IUnknown{};
struct DXGI_SWAP_CHAIN_DESC {HWND OutputWindow{};};
struct IDXGISwapChain {
    void** table{};
    HWND window=reinterpret_cast<HWND>(0x66);
    HRESULT descResult=S_OK;
    bool replaceRequestDuringDesc{};
    HRESULT GetDesc(DXGI_SWAP_CHAIN_DESC* desc){
        if(replaceRequestDuringDesc){replaceRequestDuringDesc=false;RequestNativeSize(3008,3344);}
        desc->OutputWindow=window;return descResult;
    }
};
using IDXGISwapChain3=IDXGISwapChain;
using ResizeBuffersFn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT,UINT,DXGI_FORMAT,UINT);
using ResizeBuffers1Fn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*,UINT,UINT,UINT,DXGI_FORMAT,UINT,const UINT*,IUnknown* const*);
UINT authorWidth=2048,authorHeight=2048;
int g_launcherWidth=2048,g_launcherHeight=2048;
NativeSize received{};
UINT GetForcedSwapchainWidth(){return authorWidth;}
UINT GetForcedSwapchainHeight(){return authorHeight;}
UINT GetForcedWindowWidthValue(){return authorWidth;}
UINT GetForcedWindowHeightValue(){return authorHeight;}
HWND g_gameHwnd=reinterpret_cast<HWND>(0x66);
int g_verboseLog{};
BOOL WINAPI RawRect(HWND,LPRECT r){*r={0,0,900,1024};return TRUE;}
auto g_origGetClientRect=&RawRect;
auto g_origGetWindowRect=&RawRect;
bool IsLauncherOpen(){return false;}
bool IsGameWindowForeground(HWND h){return h==g_gameHwnd;}
void Log(const char*,...);
bool IsOurOwnWindow(HWND window){return window==reinterpret_cast<HWND>(0x55);}
HRESULT STDMETHODCALLTYPE OriginalResize(IDXGISwapChain*,UINT,UINT w,UINT h,DXGI_FORMAT,UINT){received={w,h};return S_OK;}
HRESULT STDMETHODCALLTYPE OriginalResize1(IDXGISwapChain3*,UINT,UINT w,UINT h,DXGI_FORMAT,UINT,const UINT*,IUnknown* const*){received={w,h};return S_OK;}
template<class T>T GetOriginalMethod(void**,int slot){return slot==13?reinterpret_cast<T>(OriginalResize):reinterpret_cast<T>(OriginalResize1);}
void Log(const char*,...){}
void OverlayInvalidateSwapchainResources(){}
void DesktopMirrorInvalidate(){}
void CyberpunkVR_RegisterRtvSwapchain(IDXGISwapChain*){}
}
#include "ppd-consumer-sites.hpp"
static void Check(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
static void Reset(){auto s=ReadNativeApplyStatus();if(s.OwnsDimensions())CancelNativeSize(s.revision);productionConsumers::authorWidth=productionConsumers::authorHeight=2048;}
static void Resize(NativeSize expected,bool version1=false){
    productionConsumers::IDXGISwapChain sc;
    if(version1)productionConsumers::HookedResizeBuffers1(&sc,3,1920,1080,28,0,nullptr,nullptr);
    else productionConsumers::HookedResizeBuffers(&sc,3,1920,1080,28,0);
    Check(productionConsumers::received==expected,"actual ResizeBuffers consumer used a stale/mixed dimension pair");
}
static void AuthorResize(){Resize({2048,2048});Resize({2048,2048},true);productionConsumers::authorWidth=productionConsumers::authorHeight=0;Resize({1920,1080});Resize({1920,1080},true);}
static void LiveResize(){RequestNativeSize(2560,2848);Resize({2560,2848});Resize({2560,2848},true);}
static void FactoryPairs(){
    using namespace productionConsumers;DXGI_SWAP_CHAIN_DESC desc{reinterpret_cast<HWND>(0x66)};
    for(int active=0;active<2;++active){
        if(active)RequestNativeSize(2560,2848);const NativeSize expected=active?NativeSize{2560,2848}:NativeSize{2048,2048};
        Check(ActualFactory0(&desc,desc.OutputWindow)==expected,"CreateSwapChain wrapper dimension consumer mismatch");
        Check(ActualFactory1(&desc,desc.OutputWindow)==expected,"CreateSwapChain detour dimension consumer mismatch");
        Check(ActualFactory2(&desc,desc.OutputWindow)==expected,"CreateSwapChainForHwnd detour dimension consumer mismatch");
        Check(ActualFactory3(&desc,desc.OutputWindow)==expected,"CreateSwapChainForHwnd wrapper dimension consumer mismatch");
    }
}
static void ProjectionAspect(){
    Check(productionConsumers::ActualProjectionAspect()==1.0f,"author projection aspect changed");
    RequestNativeSize(2560,2848);const float actual=productionConsumers::ActualProjectionAspect();
    Check(actual==2560.0f/2848.0f,"FOV aspect remained tied to launcher after native request");
    Check(productionConsumers::g_launcherWidth==2048 && productionConsumers::g_launcherHeight==2048,"projection consumer changed launcher");
}
static void OwnWindowKeepsAuthorPath(){
    using namespace productionConsumers;RequestNativeSize(2560,2848);IDXGISwapChain sc;sc.window=reinterpret_cast<HWND>(0x55);
    HookedResizeBuffers(&sc,3,1920,1080,28,0);Check(received==NativeSize{2048,2048},"native game request hijacked mirror resize");
    HookedResizeBuffers1(&sc,3,1920,1080,28,0,nullptr,nullptr);Check(received==NativeSize{2048,2048},"native game request hijacked mirror resize1");
    DXGI_SWAP_CHAIN_DESC desc{sc.window};
    Check(ActualFactory0(&desc,sc.window)==NativeSize{2048,2048} && ActualFactory1(&desc,sc.window)==NativeSize{2048,2048} &&
          ActualFactory2(&desc,sc.window)==NativeSize{2048,2048} && ActualFactory3(&desc,sc.window)==NativeSize{2048,2048},"native game request changed own-window creation selection");
}
static void SnapshotSurvivesRevisionRace(){
    using namespace productionConsumers;RequestNativeSize(2560,2848);IDXGISwapChain sc;sc.replaceRequestDuringDesc=true;
    HookedResizeBuffers(&sc,3,1920,1080,28,0);
    Check(received==NativeSize{2560,2848} && ReadNativeRequestedSize()->size==NativeSize{3008,3344},"one resize mixed request revisions or failed snapshot fixture");
    HookedResizeBuffers1(&sc,3,1920,1080,28,0,nullptr,nullptr);Check(received==NativeSize{3008,3344},"next consumer failed to take new revision");
}
static void CancelRestoresConsumers(){RequestNativeSize(2560,2848);Resize({2560,2848});CancelNativeSize(ReadNativeApplyStatus().revision);Resize({2048,2048});Check(productionConsumers::ActualProjectionAspect()==1.0f,"cancel did not restore author aspect");}
static void VirtualClientSize(){
    RequestNativeSize(1600,1728);
    RECT rect{};productionConsumers::HookedGetClientRect(productionConsumers::g_gameHwnd,&rect);
    Check(rect.right==1600&&rect.bottom==1728,
        "game-facing virtual client dimensions still come from the old launcher resolution");
    productionConsumers::HookedGetClientRect(reinterpret_cast<HWND>(0x55),&rect);
    Check(rect.right==900&&rect.bottom==1024,"non-game physical client changed by PPD");
    productionConsumers::HookedGetWindowRect(productionConsumers::g_gameHwnd,&rect);
    Check(rect.right==1600&&rect.bottom==1728,"game-facing window rectangle still uses the old launcher resolution");
}
static void DelayedClientSize(){
    NativeApplyStatus s;s.revision=1;s.stage=NativeApplyStage::ObservedNative;s.requested=s.observedActive={976,1040};s.requestedAtMs=500;
    Check(!ReferencePpdClientSize(s,1499),"physical client changed before the original one-second delay");
    Check(ReferencePpdClientSize(s,1500)==NativeSize{960,1024},"original client width must truncate, not round");
    s.requested=s.observedActive={1968,2112};Check(ReferencePpdClientSize(s,1500)==NativeSize{954,1024},"16PPD client geometry wrong");
    s.requested=s.observedActive={1024,1024};Check(!ReferencePpdClientSize(s,1500),"original H<=1024 no-op lost");
    s.requested={1968,2112};Check(!ReferencePpdClientSize(s,1500),"unobserved game size shrank the window");
}
int main(){unsigned pass=0,fail=0;const std::pair<const char*,void(*)()> tests[]={
    {"virtual_client_size",VirtualClientSize},
    {"delayed_client_size",DelayedClientSize},
    {"consumer_author_resize",AuthorResize},{"consumer_native_resize",LiveResize},{"consumer_factories",FactoryPairs},
    {"consumer_aspect",ProjectionAspect},{"consumer_own_window",OwnWindowKeepsAuthorPath},{"consumer_revision_race",SnapshotSurvivesRevisionRace},{"consumer_cancel",CancelRestoresConsumers}};
    for(const auto& [name,test]:tests){try{Reset();test();++pass;std::cout<<"PASS "<<name<<'\n';}catch(const std::exception& e){++fail;std::cout<<"FAIL "<<name<<": "<<e.what()<<'\n';}}
    Reset();std::cout<<"RESULT "<<pass<<" passed, "<<fail<<" failed\n";return fail?1:0;}

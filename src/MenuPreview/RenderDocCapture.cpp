#include "MenuPreview/RenderDocCapture.hpp"
#include "MenuPreview/RenderDocCapturePolicy.hpp"
#include "MenuPreview/RenderDocApp.h"
#include "MenuPreview/Scope.hpp"
#include "Utils/StereoLog.hpp"
#include <d3d12.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <chrono>
#include <iomanip>
#include <limits>

namespace cvr::menu_preview {
namespace {
using Microsoft::WRL::ComPtr;
std::mutex captureMutex;
std::filesystem::path MarkerPath(){wchar_t value[32768]{};const auto n=GetEnvironmentVariableW(L"CPVR_MENU_RENDERDOC_REQUEST",value,32768);return n&&n<32768?std::filesystem::path(value):std::filesystem::path{};}
const auto marker=MarkerPath();
bool attempted{},capturing{};
RENDERDOC_API_1_6_0* api{};
ComPtr<IDXGISwapChain> captureChain;ComPtr<ID3D12Device> captureDevice;HWND captureWindow{};
bool WindowMatchRequested(){static const bool enabled=[] {char value[4]{};return GetEnvironmentVariableA("CPVR_RENDERDOC_WINDOW_MATCH",value,sizeof(value))==1&&value[0]=='1';}();return enabled;}
void* ApiDevice(){return WindowMatchRequested()?nullptr:captureDevice.Get();}
CaptureRequestIdentity request;uint32_t beforeCaptures{};
uint64_t UnixNow(){return uint64_t(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());}
uint64_t Creation(){FILETIME c{},e{},k{},u{};if(!GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u))return 0;return (uint64_t(c.dwHighDateTime)<<32)|c.dwLowDateTime;}
std::string Run(){char value[128]{};const auto n=GetEnvironmentVariableA("CPVR_MENU_TEST_RUN_ID",value,128);return n&&n<128?std::string(value,n):std::string{};}
void Result(const char* status,const char* reason,bool success=false){
    const auto path=marker.parent_path()/L"capture-api-result.json";
    std::ofstream out(path,std::ios::trunc);
    out<<"{\"status\":\""<<status<<"\",\"reason\":\""<<reason<<"\",\"pid\":"<<GetCurrentProcessId()
       <<",\"start_filetime\":"<<Creation()<<",\"preview_epoch\":"<<request.previewEpoch<<",\"preview_screen\":"<<request.previewScreen
       <<",\"preview_key\":\""<<std::hex<<std::setw(16)<<std::setfill('0')<<request.key<<std::dec<<"\""
       <<",\"window_match\":"<<(WindowMatchRequested()?"true":"false")
       <<",\"capture_success\":"<<(success?"true":"false")<<",\"before_count\":"<<beforeCaptures
       <<",\"after_count\":"<<(api?api->GetNumCaptures():0)<<"}";
    Log("[MenuPreviewCapture] %s reason=%s pid=%u chain=%p device=%p hwnd=%p; one real game Present interval, no portrait role assignment\n",
        status,reason,GetCurrentProcessId(),captureChain.Get(),captureDevice.Get(),captureWindow);
}
bool ReadRequest(){
    if(!std::filesystem::exists(marker)||std::filesystem::file_size(marker)>4096)return false;
    std::ifstream in(marker);std::unordered_map<std::string,std::string> values;std::string line;
    while(std::getline(in,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();const auto split=line.find('=');if(split==std::string::npos)return false;values[line.substr(0,split)]=line.substr(split+1);}
    request.pid=uint32_t(std::stoul(values.at("pid")));request.creation=std::stoull(values.at("start_filetime"));
    request.expires=std::stoull(values.at("expires_unix"));request.previewEpoch=uint32_t(std::stoul(values.at("preview_epoch")));
    request.previewScreen=1; // Optional field: legacy Inventory requests remain screen1.
    if(const auto i=values.find("preview_screen");i!=values.end()){
        size_t consumed{};const auto screen=std::stoul(i->second,&consumed,10);
        if(consumed!=i->second.size()||screen>std::numeric_limits<uint32_t>::max())return false;
        request.previewScreen=uint32_t(screen);
    }
    request.key=std::stoull(values.at("preview_key"),nullptr,16);request.run=values.at("run_id");
    return values.at("state")=="OPEN_STABLE"&&ValidCaptureRequest(request,GetCurrentProcessId(),Creation(),Run(),UnixNow());
}
bool ScopeCurrent(){Lease lease;return ValidCaptureRequest(request,GetCurrentProcessId(),Creation(),Run(),UnixNow())&&
    TryAcquireForVirtualCamera(request.key,lease)&&ValidCaptureScope(request,lease.metadata.screen,lease.metadata.screenEpoch,lease.metadata.virtualCameraNameHash);}
void Discard(const char* reason){
    if(capturing&&api)api->DiscardFrameCapture(ApiDevice(),captureWindow);
    capturing=false;Result("REJECTED",reason);captureChain.Reset();captureDevice.Reset();
}
}
void RenderDocAfterGamePresent(IDXGISwapChain* chain,HRESULT presentResult)noexcept{
    if(marker.empty())return;
    try{
        std::lock_guard lock(captureMutex);
        if(capturing){
            if(chain!=captureChain.Get()){Discard("game_chain_changed");return;}
            DXGI_SWAP_CHAIN_DESC d{};ComPtr<ID3D12Device> device;
            if(FAILED(presentResult)||FAILED(chain->GetDesc(&d))||d.OutputWindow!=captureWindow||
               FAILED(chain->GetDevice(IID_PPV_ARGS(&device)))||device.Get()!=captureDevice.Get()||!ScopeCurrent()){
                Discard("game_pair_or_preview_scope_invalidated");return;
            }
            const auto ended=api->EndFrameCapture(ApiDevice(),captureWindow);capturing=false;
            const bool success=ended==1&&api->GetNumCaptures()>beforeCaptures;
            Result(success?"PASS":"FAIL",success?"actual_game_frame_finalized":"EndFrameCapture_failed",success);
            captureChain.Reset();captureDevice.Reset();return;
        }
        if(attempted||FAILED(presentResult)||!std::filesystem::exists(marker))return;
        attempted=true;
        if(!ReadRequest()||!ScopeCurrent()){Result("REJECTED","request_identity_or_preview_scope_invalid");return;}
        DXGI_SWAP_CHAIN_DESC d{};
        if(!chain||FAILED(chain->GetDesc(&d))||!d.OutputWindow||FAILED(chain->GetDevice(IID_PPV_ARGS(&captureDevice)))){Result("FAIL","exact_game_pair_unavailable");return;}
        DWORD ownerPid{};GetWindowThreadProcessId(d.OutputWindow,&ownerPid);
        if(ownerPid!=GetCurrentProcessId()){Result("REJECTED","game_window_owner_mismatch");captureDevice.Reset();return;}
        const auto module=GetModuleHandleW(L"renderdoc.dll");
        const auto get=module?reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module,"RENDERDOC_GetAPI")):nullptr;
        if(!get||get(eRENDERDOC_API_Version_1_6_0,reinterpret_cast<void**>(&api))!=1||!api){Result("FAIL","public_RenderDoc_API_unavailable");captureDevice.Reset();return;}
        if(api->IsFrameCapturing()){Result("REJECTED","another_capture_already_active");captureDevice.Reset();return;}
        captureChain=chain;captureWindow=d.OutputWindow;beforeCaptures=api->GetNumCaptures();
        const auto capturePath=(marker.parent_path()/L"frame").u8string();
        api->SetCaptureFilePathTemplate(reinterpret_cast<const char*>(capturePath.c_str()));
        // Public API permits a null device with an exact window. The opt-in
        // addresses outer device wrappers while retaining the verified game
        // HWND and same-chain/device/PID/scope guards; never wildcard the HWND.
        // SetActiveWindow is intentionally not used (it requires both arguments).
        api->StartFrameCapture(ApiDevice(),captureWindow);capturing=api->IsFrameCapturing()==1;
        if(!capturing){Result("FAIL","StartFrameCapture_not_active");captureChain.Reset();captureDevice.Reset();return;}
        Result("CAPTURING","actual_game_pair_started");
    }catch(...){
        try{std::lock_guard lock(captureMutex);if(capturing)Discard("capture_bridge_exception");else{attempted=true;Result("FAIL","capture_bridge_exception");}}catch(...){}
    }
}
}

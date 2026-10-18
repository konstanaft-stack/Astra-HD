#include "Hooks/GpuObjectProvenance.hpp"
#include <d3d12.h>
#include <wrl/client.h>
#include <atomic>
#include <cwchar>
#include <iterator>

void NameGpuObjectProvenance(IUnknown* value,const wchar_t* kind,const void* caller) noexcept {
    static const bool enabled=[] {wchar_t flag[2]{};return GetEnvironmentVariableW(
        L"CPVR_D3D12_VALIDATION",flag,2)==1&&flag[0]==L'1';}();
    if(!enabled||!value)return;
    Microsoft::WRL::ComPtr<ID3D12Object> object;
    if(FAILED(value->QueryInterface(IID_PPV_ARGS(&object))))return;
    static std::atomic<unsigned long long> serial{};
    wchar_t name[640]{};int used=swprintf_s(name,L"CPVR_DIAG_%s_%llu",kind,++serial);
    void* frames[8]{};const auto depth=CaptureStackBackTrace(2,8,frames,nullptr);
    // Include the immediate client and the next distinct module. This preserves
    // the real allocation owner when a Streamline device proxy is in between.
    HMODULE last{};unsigned modules{};
    for(int i=-1;i<int(depth)&&modules<3;++i){
        const void* address=i<0?caller:frames[i];HMODULE module{};wchar_t path[MAX_PATH]{};
        if(!address||!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address),&module)||module==last)continue;
        if(!GetModuleFileNameW(module,path,MAX_PATH))continue;
        const auto* slash=wcsrchr(path,L'\\');const auto* base=slash?slash+1:path;
        const auto written=_snwprintf_s(name+used,std::size(name)-size_t(used),_TRUNCATE,L" %s+%llX",base,
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address)-reinterpret_cast<uintptr_t>(module)));
        if(written<0)break;used+=written;last=module;++modules;
    }
    object->SetName(name);
}

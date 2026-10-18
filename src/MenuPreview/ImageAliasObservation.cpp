#include "MenuPreview/ImageAliasObservation.hpp"
#include "MenuPreview/ProducerRoute.hpp"
#include "Hooks/Hook.hpp"
#include "Utils/StereoLog.hpp"
#include <MinHook.h>
#include <windows.h>
#include <chrono>
#include <cstring>

extern "C" {
void* CvrImageAliasAssignOriginal{};
uintptr_t CvrImageAliasAssignCaller{};
void CvrImageAliasAssignDetour();
}
namespace {
bool ReadAlias(uintptr_t pointer,void* out,size_t size){
    __try {if(pointer<0x10000)return false;std::memcpy(out,reinterpret_cast<const void*>(pointer),size);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool Enabled(){char value[4]{};return GetEnvironmentVariableA("CPVR_MENU_IMAGE_ALIAS_TRACE",value,sizeof(value))==1 && value[0]=='1';}
bool InstallImageAlias(){
    const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if(!base)return false;
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    static constexpr uint8_t prologue[]{0x40,0x53,0x48,0x83,0xEC,0x30,0x48,0x8B,0xD9,0x48,0x8D,0x44,0x24,0x20,0x33,0xC9};
    static constexpr uint8_t call[]{0xE8,0xB3,0x2D,0xEF,0xFE};
    if(nt->FileHeader.TimeDateStamp!=0x68AF45EA || nt->OptionalHeader.SizeOfImage!=0x4EFC000 ||
       std::memcmp(reinterpret_cast<const void*>(base+0x100A8C),prologue,sizeof(prologue)) ||
       std::memcmp(reinterpret_cast<const void*>(base+0x120DCD4),call,sizeof(call)))return false;
    CvrImageAliasAssignCaller=base+0x120DCD9;
    const auto created=MH_CreateHook(reinterpret_cast<void*>(base+0x100A8C),reinterpret_cast<void*>(&CvrImageAliasAssignDetour),&CvrImageAliasAssignOriginal);
    if(created!=MH_OK){Log("[MenuPreview] image-alias assignment hook create failed=%d\n",int(created));return false;}
    const auto enabled=MH_EnableHook(reinterpret_cast<void*>(base+0x100A8C));
    if(enabled!=MH_OK){MH_RemoveHook(reinterpret_cast<void*>(base+0x100A8C));CvrImageAliasAssignOriginal=nullptr;return false;}
    Log("[MenuPreview] image-alias trace armed assignment=100A8C caller=120DCD9; optional process-only CPU trace\n");return true;
}
}
extern "C" void CvrImageAliasObserve(uintptr_t assignmentSlot,uintptr_t sourceRef)noexcept {
    try{
        using namespace cvr::menu_preview;
        // Exact inkImage dynamic-load callback holds strong backend/resource
        // refs here, BEFORE its existing assignment. Never lock its weak closure.
        if(assignmentSlot<0x10068)return;
        auto row=std::make_shared<ImageAliasObservation>();row->assignmentSlot=assignmentSlot;row->backend=assignmentSlot-0x68;
        if(!ReadAlias(sourceRef,&row->resource,sizeof(row->resource)) ||
           !ReadAlias(row->backend+0x110,&row->nameHash,sizeof(row->nameHash)))return;
        if(row->resource)ReadAlias(row->resource+0x30,&row->resourcePath,sizeof(row->resourcePath));
        Lease lease;row->scopeKnown=TryAcquireCurrent(lease);
        if(row->scopeKnown){row->metadata=lease.metadata;
            if(!ReadAlias(lease.metadata.componentIdentity+0x1E8,&row->componentResource,sizeof(row->componentResource)))return;
            Lease current;
            row->scopeKnown=TryAcquireCurrent(current) && SamePreview(current.metadata,lease.metadata);
        }
        row->aliasMatch=ExactImageResourceAlias(row->resource,row->componentResource,row->scopeKnown);
        static std::atomic<uint64_t> sequence{};row->sequence=sequence.fetch_add(1,std::memory_order_relaxed)+1;
        row->observedAtMs=uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        static std::atomic<uint32_t> matches{},candidates{};
        const bool emit=row->aliasMatch?matches.fetch_add(1,std::memory_order_relaxed)<32:candidates.fetch_add(1,std::memory_order_relaxed)<16;
        if(emit)Log("[MenuPreview] ink-image-alias seq=%llu epoch=%u setup=%u backend=%llx name=%llx resourceCPU=%llx path=%llx component=%llx previewResourceCPU=%llx scopeKnown=%u aliasMatch=%u; engine-owned callback references only, no GPU/UI generation claim\n",
            row->sequence,row->metadata.screenEpoch,row->metadata.setupID,row->backend,row->nameHash,row->resource,row->resourcePath,
            row->metadata.componentIdentity,row->componentResource,unsigned(row->scopeKnown),unsigned(row->aliasMatch));
        // Candidate-before-scope is log-only. It cannot become proof by address
        // reuse or later dereferencing a raw backend pointer.
        if(row->aliasMatch)imageAliasObservation.store(std::move(row),std::memory_order_release);
    }catch(...){/* Observation must not alter native assignment behaviour. */}
}
CVR_HOOK_IF("MenuImageAlias",::cvr::hooks::Stage::Boot,84,InstallImageAlias,Enabled);

#include "MenuPreview/ReaderObservation.hpp"
#include "Framegen/Inputs.hpp"
#include "Hooks/Hook.hpp"
#include "Utils/StereoLog.hpp"
#include <windows.h>
#include <MinHook.h>
#include <chrono>
#include <cstring>

// Existing dispatcher TLS; no context pointer is manufactured from a name.
namespace cvr::detail {
extern thread_local uintptr_t t_current_node_work;
extern thread_local bool t_active_view_known;
extern thread_local uint64_t t_active_view_key;
}
namespace {
using Getter=uintptr_t(__fastcall*)(uintptr_t resource,uintptr_t outputRef);
Getter original{};uintptr_t exe{};
bool Read(uintptr_t pointer,void* out,size_t size){
    __try {if(pointer<0x10000)return false;std::memcpy(out,reinterpret_cast<const void*>(pointer),size);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void Observe(uintptr_t resource,uintptr_t outputRef,uintptr_t caller){
    using namespace cvr::menu_preview;
    const auto alias=ReadWidgetAliasObservation();Lease lease;
    if(!alias || !TryAcquireCurrent(lease) || !ExactReaderAlias(*alias,lease.metadata,resource)){
        static std::atomic<uint32_t> rejected{};
        if(rejected.fetch_add(1,std::memory_order_relaxed)<8)
            Log("[MenuPreview] reader-ref rejected resourceCPU=%llx reason=no-current-typed-widget-alias; candidate not admitted\n",resource);
        return;
    }
    auto row=std::make_shared<ReaderObservation>();row->metadata=lease.metadata;row->aliasSequence=alias->sequence;
    row->resource=resource;row->outputRef=outputRef;row->caller=caller;
    uintptr_t readableField{},componentResource{};
    if(!Read(outputRef,&row->readBacking,sizeof(row->readBacking)) || !row->readBacking ||
       !Read(resource+0x70,&readableField,sizeof(readableField)) ||
       !Read(resource+0x78,&row->writeBacking,sizeof(row->writeBacking)) ||
       !Read(lease.metadata.componentIdentity+0x1E8,&componentResource,sizeof(componentResource)))return;
    row->readFieldMatch=row->readBacking==readableField;
    Lease current;
    if(!row->readFieldMatch || componentResource!=resource || !TryAcquireCurrent(current) ||
       !ExactReaderAlias(*alias,current.metadata,resource))return;
    row->nodeWork=cvr::detail::t_current_node_work;
    row->nodeKnown=row->nodeWork && cvr::detail::t_active_view_known;
    if(row->nodeKnown)row->nodeKey=cvr::detail::t_active_view_key;
    row->clockKnown=cvr::framegen::ReadRenderFrameIndex(&row->rendererClock);
    row->observedAtMs=uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    static std::atomic<uint64_t> sequence{};row->sequence=sequence.fetch_add(1,std::memory_order_relaxed)+1;
    static std::atomic<uint32_t> rows{};
    if(rows.fetch_add(1,std::memory_order_relaxed)<32)
        Log("[MenuPreview] reader-ref epoch=%u setup=%u resourceCPU=%llx read70=%llx write78=%llx sameBacking=%u aliasSeq=%llu caller=%llx nodeKnown=%u node=%llx nodeKey=%llx clockKnown=%u rendererClock=%u; existing owned accessor ref, NOT completed UI/GPU generation\n",
            row->metadata.screenEpoch,row->metadata.setupID,row->resource,row->readBacking,row->writeBacking,unsigned(row->readBacking==row->writeBacking),
            row->aliasSequence,caller,unsigned(row->nodeKnown),row->nodeWork,row->nodeKey,unsigned(row->clockKnown),row->rendererClock);
    readerObservation.store(std::move(row),std::memory_order_release);
}
uintptr_t __fastcall Reader(uintptr_t resource,uintptr_t outputRef){
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    // Source-proven virtual ITexture getter: RCX=this, RDX=outRef*, RAX=outRef*.
    // Original may create its backing lazily; never issue a second getter call.
    const auto result=original(resource,outputRef);
    if(caller==exe+0x2F3A86 && result==outputRef){
        try {Observe(resource,result,caller);}catch(...){
            static std::atomic<uint32_t> errors{};if(errors.fetch_add(1,std::memory_order_relaxed)<4)
                try {Log("[MenuPreview] reader-ref observation exception; original getter result preserved\n");}catch(...){}
        }
    }
    return result;
}
bool Enabled(){char value[4]{};return GetEnvironmentVariableA("CPVR_MENU_READER_TRACE",value,sizeof(value))==1 && value[0]=='1';}
bool InstallReader(){
    exe=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));if(!exe)return false;
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(exe+dos->e_lfanew);
    static constexpr uint8_t prefix[]{0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57};
    static constexpr uint8_t call[]{0xFF,0x90,0x48,0x01,0x00,0x00};
    if(nt->FileHeader.TimeDateStamp!=0x68AF45EA || nt->OptionalHeader.SizeOfImage!=0x4EFC000 ||
       std::memcmp(reinterpret_cast<const void*>(exe+0x2F3F58),prefix,sizeof(prefix)) ||
       std::memcmp(reinterpret_cast<const void*>(exe+0x2F3A80),call,sizeof(call)))return false;
    const auto created=MH_CreateHook(reinterpret_cast<void*>(exe+0x2F3F58),reinterpret_cast<void*>(&Reader),reinterpret_cast<void**>(&original));
    if(created!=MH_OK)return false;
    if(MH_EnableHook(reinterpret_cast<void*>(exe+0x2F3F58))!=MH_OK){MH_RemoveHook(reinterpret_cast<void*>(exe+0x2F3F58));original=nullptr;return false;}
    Log("[MenuPreview] reader-ref trace armed getter=2F3F58 caller=2F3A86; optional existing accessor only\n");return true;
}
}
CVR_HOOK_IF("MenuReaderRef",::cvr::hooks::Stage::Boot,86,InstallReader,Enabled);

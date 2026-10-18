#include "Natives/NativeFunctions.hpp"
#include "MenuPreview/WidgetAliasObservation.hpp"
#include "MenuPreview/UiConsumer.hpp"
#include "MenuPreview/ProducerRoute.hpp"
#include "Utils/StereoLog.hpp"
#include <RED4ext/Scripting/Natives/Generated/ink/ImageWidget.hpp>
#include <RED4ext/Scripting/Natives/Generated/game/ui/PuppetPreviewGameController.hpp>
#include <windows.h>
#include <chrono>
#include <cstring>
namespace {
bool Enabled(){static const bool enabled=[] {
    char v[4]{};if((GetEnvironmentVariableA("CPVR_MENU_WIDGET_ALIAS_TRACE",v,sizeof(v))!=1 || v[0]!='1')&&!cvr::menu_preview::UiConsumerEnabled())return false;
    const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));if(!base)return false;
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    return nt->FileHeader.TimeDateStamp==0x68AF45EA && nt->OptionalHeader.SizeOfImage==0x4EFC000;
}();return enabled;}
bool ReadWidgetBytes(uintptr_t p,void* out,size_t size){
    __try {if(p<0x10000)return false;std::memcpy(out,reinterpret_cast<const void*>(p),size);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
}
void VRPreviewWidgetAliasEnabled(RED4ext::IScriptable*,RED4ext::CStackFrame* frame,bool* out,int64_t){++frame->code;if(out)*out=Enabled();}
static void ObserveTypedImageImpl(RED4ext::CStackFrame* frame,bool* out){
    RED4ext::Handle<RED4ext::game::ui::PuppetPreviewGameController> controller;
    RED4ext::Handle<RED4ext::ink::ImageWidget> image;uint32_t epoch{};
    RED4ext::GetParameter(frame,&controller);RED4ext::GetParameter(frame,&epoch);RED4ext::GetParameter(frame,&image);++frame->code;
    if(out)*out=false;if(!Enabled() || !controller || !image)return;
    using namespace cvr::menu_preview;
    Lease lease;if(!TryAcquireCurrent(lease) || lease.metadata.screenEpoch!=epoch ||
       lease.metadata.controllerIdentity!=reinterpret_cast<uintptr_t>(controller.GetPtr()) ||
       controller->cameraController.activeSetup!=lease.metadata.setupID)return;
    const auto* expected=RED4ext::CRTTISystem::Get()->GetClass(RED4ext::CName("inkImageWidget"));
    if(!expected || !image->GetType() || !image->GetType()->IsA(expected))return;
    static_assert(offsetof(RED4ext::ink::ImageWidget,externalDynamicTexture)==0x210);
    static_assert(offsetof(RED4ext::ink::ImageWidget,useExternalDynamicTexture)==0x224);
    auto row=std::make_shared<WidgetAliasObservation>();row->metadata=lease.metadata;row->widget=reinterpret_cast<uintptr_t>(image.GetPtr());
    row->externalName=uint64_t(image->externalDynamicTexture);row->useExternal=image->useExternalDynamicTexture;
    // Actual inkImage method/callback pins prove backend260/resource68. Strong
    // widget/controller arguments are held for this game/script callback only.
    if(!ReadWidgetBytes(row->widget+0x260,&row->backend,sizeof(row->backend)) || !row->backend ||
       !ReadWidgetBytes(row->backend+0x68,&row->resource,sizeof(row->resource)) ||
       !ReadWidgetBytes(row->backend+0x110,&row->backendName,sizeof(row->backendName)) ||
       !ReadWidgetBytes(lease.metadata.componentIdentity+0x1E8,&row->componentResource,sizeof(row->componentResource)))return;
    uintptr_t backendAgain{},resourceAgain{},previewAgain{};Lease current;
    row->snapshotStable=ReadWidgetBytes(row->widget+0x260,&backendAgain,sizeof(backendAgain)) && backendAgain==row->backend &&
        ReadWidgetBytes(row->backend+0x68,&resourceAgain,sizeof(resourceAgain)) && resourceAgain==row->resource &&
        ReadWidgetBytes(lease.metadata.componentIdentity+0x1E8,&previewAgain,sizeof(previewAgain)) && previewAgain==row->componentResource;
    const bool currentScope=TryAcquireCurrent(current) && SamePreview(current.metadata,lease.metadata) &&
        current.metadata.controllerIdentity==lease.metadata.controllerIdentity;
    row->aliasMatch=ExactWidgetResourceAlias(row->resource,row->componentResource,currentScope,row->snapshotStable);
    if(row->aliasMatch)row->backingFieldsKnown=ReadWidgetBytes(row->resource+0x70,&row->currentReadBacking,sizeof(row->currentReadBacking)) &&
        ReadWidgetBytes(row->resource+0x78,&row->currentWriteBacking,sizeof(row->currentWriteBacking));
    static std::atomic<uint64_t> sequence{};row->sequence=sequence.fetch_add(1,std::memory_order_relaxed)+1;
    row->observedAtMs=uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    static std::atomic<uint32_t> matchedRows{},candidateRows{};
    const bool emit=row->aliasMatch?matchedRows.fetch_add(1,std::memory_order_relaxed)<32:candidateRows.fetch_add(1,std::memory_order_relaxed)<16;
    if(emit)Log("[MenuPreview] typed-widget-alias epoch=%u setup=%u widget=%llx backend=%llx externalName=%llx useExternal=%u backendName=%llx resourceCPU=%llx previewResourceCPU=%llx stable=%u aliasMatch=%u fieldsKnown=%u currentRead70=%llx currentWrite78=%llx; current fields ONLY, not accessor/consumption event or GPU generation\n",
        epoch,row->metadata.setupID,row->widget,row->backend,row->externalName,unsigned(row->useExternal),row->backendName,
        row->resource,row->componentResource,unsigned(row->snapshotStable),unsigned(row->aliasMatch),unsigned(row->backingFieldsKnown),row->currentReadBacking,row->currentWriteBacking);
    if(row->aliasMatch){ObserveUiOwnedAlias(row->metadata,row->backend,row->resource);widgetAliasObservation.store(row,std::memory_order_release);}
    if(out)*out=row->aliasMatch;
}
void VRPreviewObserveTypedImage(RED4ext::IScriptable*,RED4ext::CStackFrame* frame,bool* out,int64_t){
    if(out)*out=false;
    try {ObserveTypedImageImpl(frame,out);}
    catch(...){
        static std::atomic<uint32_t> errors{};
        if(errors.fetch_add(1,std::memory_order_relaxed)<4){
            try {Log("[MenuPreview] typed-widget-alias exception; observation rejected, game callback preserved\n");}catch(...){}
        }
    }
}

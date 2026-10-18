#pragma once
#include "MenuPreview/CameraApplication.hpp"
#include "Camera/ImagePoseLedger.hpp"

namespace cvr::menu_preview {
using NodeBindingSlot=std::shared_ptr<const CameraApplication>;
// POD token permits use from the SEH-heavy dispatcher without introducing a
// C++ unwinding object into a function containing __try blocks.
struct PreviewNodeScopeToken {size_t depth{};size_t pixelDepth{};};
class NodeBindingStack {
public:
    PreviewNodeScopeToken Enter(NodeBindingSlot next,uint32_t workRva=0)noexcept{
        try{stack_.push_back({current_,workRva_});current_=std::move(next);workRva_=workRva;return {stack_.size()};}
        catch(...){current_={};workRva_=0;return {};}
    }
    void Restore(PreviewNodeScopeToken token)noexcept{
        if(!token.depth)return;
        if(token.depth!=stack_.size()){current_={};workRva_=0;return;}
        current_=std::move(stack_.back().first);workRva_=stack_.back().second;stack_.pop_back();
    }
    const NodeBindingSlot& Current()const noexcept{return current_;}
    uint32_t WorkRva()const noexcept{return workRva_;}
private:NodeBindingSlot current_;uint32_t workRva_{};std::vector<std::pair<NodeBindingSlot,uint32_t>> stack_;
};
inline bool MatchPreviewNode(const NodeBindingSlot& app,uintptr_t context,uint64_t key,
    bool clockKnown,uint32_t frame,uint64_t now) noexcept {
    if(!app||!context||!key||!clockKnown||!app->frame||app->frame.eye>1)return false;
    const auto& m=app->metadata;
    const PreviewFrameKey scope{m.screenEpoch,m.setupID,m.componentIdentity,m.virtualCameraNameHash};
    const bool source=app->source.mode!=NativePreviewMode::NativeEditorDual||
        (app->source.component&&app->source.cameraKey&&app->source.bindingGeneration&&app->source.eye==app->frame.eye);
    return source&&SupportedPreviewScreen(m.screen)&&m.screenEpoch&&m.componentIdentity&&FreshPublication(m.publishedAtMs,now)&&
        app->nativeContext==context&&key==CameraApplicationSourceKey(*app)&&app->frame.key==scope&&app->frame.nativeFrame==frame;
}
// Submitted RTV BIND metadata, not proof of a draw, completed GPU work or UI read.
// The generic ledger owns command-list/resource lifetimes and submit generation.
struct RtvBindingPayload {
    NodeBindingSlot application;
    uint32_t workRva{};uint64_t width{};uint32_t height{},format{},mips{},array{},samples{};bool descriptorKnown{};
    explicit operator bool()const noexcept{return bool(application);}
    const CameraApplication* operator->()const noexcept{return application.get();}
    bool operator==(const RtvBindingPayload&)const=default;
    friend bool operator==(const RtvBindingPayload& p,const NodeBindingSlot& a){return p.application==a;}
};
class ImageLineageStore {
    using Ledger=camera::ImagePoseLedger<RtvBindingPayload>;
public:
    using Prepared=Ledger::Prepared;using Image=Ledger::Image;
    void ObserveScope(const NodeBindingSlot& current){
        std::lock_guard lock(mutex_);
        const auto next=current?std::optional(current->frame.key):std::nullopt;
        if(next!=scope_){scope_=next;ledger_.InvalidatePublished();}
    }
    void Record(uintptr_t list,uintptr_t resource,NodeBindingSlot app){
        ledger_.Record(list,resource,RtvBindingPayload{app},false);
    }
    void RecordBinding(uintptr_t list,uintptr_t resource,const RtvBindingPayload& payload){ledger_.Record(list,resource,payload,false);}
    void Reset(uintptr_t list){ledger_.Reset(list);}
    Prepared Prepare(uintptr_t queue,std::span<const uintptr_t> lists)noexcept{return ledger_.Prepare(queue,lists);}
    template<class ListAt> Prepared Prepare(uintptr_t queue,size_t n,ListAt&& at)noexcept{
        return ledger_.Prepare(queue,n,std::forward<ListAt>(at));
    }
    void Commit(uintptr_t queue,Prepared&& p,uint64_t now)noexcept{ledger_.Commit(queue,std::move(p),now);}
    Image Read(uintptr_t resource)const{
        std::lock_guard lock(mutex_);auto image=ledger_.Read(resource);
        if(!image||!image.pose||!scope_||image.pose->frame.key!=*scope_)return {};
        return image;
    }
    bool Validate(const Image& image,uintptr_t queue)const{
        if(!image||!image.pose||image.queue!=queue)return false;
        const auto current=Read(image.resource);
        return current&&current.queue==queue&&current.generation==image.generation&&current.pose==image.pose;
    }
    void Invalidate(){ledger_.InvalidatePublished();}
    void ForgetResource(uintptr_t resource){ledger_.ForgetResource(resource);}
    void ForgetList(uintptr_t list){ledger_.ForgetList(list);}
private:
    mutable std::mutex mutex_;std::optional<PreviewFrameKey> scope_;Ledger ledger_;
};
}

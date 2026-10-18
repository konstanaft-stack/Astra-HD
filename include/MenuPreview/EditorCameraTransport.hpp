#pragma once
#include "Camera/PoseAddressLedger.hpp"
#include "MenuPreview/CameraApplication.hpp"
#include <limits>

namespace cvr::menu_preview {
// Preparation proves which original native camera was copied. It contains no
// tracked projection/context and is never admitted as a rendered application.
inline bool IsEditorNativePreparation(const CameraApplication& app)noexcept {
    return app.source.mode==NativePreviewMode::NativeEditorDual&&app.source.eye<2&&
        app.frame&&app.frame.eye==app.source.eye&&!app.projection.valid&&!app.nativeContext&&
        app.baseDescriptor==app.eyeDescriptor;
}
inline bool EditorNativeSetupCaptureAgeAllowed(const CameraApplication& app,uint64_t now)noexcept {
    const auto published=app.metadata.publishedAtMs;
    // Original native packets may remain cached while fresh tracked views are
    // built from a current Lease. Address/source proof is still required by
    // the caller; this exception grants no tracked image or GPU freshness.
    return published&&now>=published&&
        (IsEditorNativePreparation(app)||FreshPublication(published,now));
}
// This is the native setup layout, not the temporary Descriptor layout.
inline cvr::camera::PoseFingerprint EditorSetupFingerprint(const Descriptor& descriptor)noexcept {
    cvr::camera::PoseFingerprint value;
    std::memcpy(value.position.data(),descriptor.data()+16,12);
    std::memcpy(value.rotation.data(),descriptor.data(),16);
    return value;
}
inline bool MatchesEditorRenderedDescriptor(const CameraApplication& app,const Descriptor& descriptor)noexcept {
    // The native registered-view conversion owns auxiliary descriptor fields.
    // Only the pose and projection fields written at registration must be
    // bit-identical; do not compare unrelated reconstructed/padding bytes.
    return EditorSetupFingerprint(app.eyeDescriptor)==EditorSetupFingerprint(descriptor)&&
        std::memcmp(app.eyeDescriptor.data()+0x38,descriptor.data()+0x38,12)==0;
}
class EditorCameraTransport final {
    using Ledger=cvr::camera::PoseAddressLedger<std::shared_ptr<const CameraApplication>,256>;
    Ledger addresses_;
public:
    using Receipt=Ledger::Receipt;
    void Invalidate(uintptr_t address){addresses_.Invalidate(address);}
    bool Publish(uintptr_t address,const cvr::camera::PoseFingerprint& actual,
                 std::shared_ptr<const CameraApplication> app){
        if(!app||app->source.mode!=NativePreviewMode::NativeEditorDual||app->source.eye>1||
           !app->source.component||!app->source.cameraKey||!app->source.bindingGeneration||
           app->frame.eye!=app->source.eye||!app->frame||actual!=EditorSetupFingerprint(app->eyeDescriptor)){
            addresses_.Invalidate(address);return false;
        }
        addresses_.Publish(address,actual,app);return true;
    }
    Receipt Capture(uintptr_t address,const cvr::camera::PoseFingerprint& actual)const{return addresses_.Capture(address,actual);}
    bool Transfer(const Receipt& before,const cvr::camera::PoseFingerprint& sourceNow,
                  uintptr_t destination,const cvr::camera::PoseFingerprint& destinationNow){
        if(!before||!before.payload||destinationNow!=EditorSetupFingerprint(before.payload->eyeDescriptor)){
            addresses_.Invalidate(destination);return false;
        }
        return addresses_.Transfer(before,sourceNow,destination,destinationNow);
    }
};

// One synchronous native-copy bracket owns only this scalar outside the TLS
// store. In particular, the SEH WeaponAim hook must not hold a shared_ptr or a
// nontrivial Receipt local. Slots are consumed even when native proof fails.
template<size_t Capacity=8> class EditorCameraCopyTicketStore final {
    static_assert(Capacity>0);
    struct Pending {uint64_t ticket{};EditorCameraTransport::Receipt receipt;};
    std::array<Pending,Capacity> pending_{};
    uint64_t sequence_{};
public:
    explicit EditorCameraCopyTicketStore(uint64_t lastTicket=0)noexcept:sequence_(lastTicket){}
    uint64_t Begin(const EditorCameraTransport::Receipt& before)noexcept {
        if(!before||!before.payload||before.payload->source.mode!=NativePreviewMode::NativeEditorDual||
           before.payload->source.eye>1||sequence_==std::numeric_limits<uint64_t>::max())return 0;
        for(auto& pending:pending_)if(!pending.ticket){
            pending.ticket=++sequence_;pending.receipt=before;return pending.ticket;
        }
        return 0;
    }
    template<class ReadFingerprint>
    bool Complete(uint64_t ticket,EditorCameraTransport& transport,uintptr_t destination,
        ReadFingerprint&& read,EditorCameraTransport::Receipt* completed=nullptr) {
        if(completed)*completed={};
        EditorCameraTransport::Receipt before;
        if(ticket)for(auto& pending:pending_)if(pending.ticket==ticket){
            before=std::move(pending.receipt);pending={};break;
        }
        if(completed)*completed=before;
        cvr::camera::PoseFingerprint sourceNow,destinationNow;
        if(!before||!read(before.source,sourceNow)||!read(destination,destinationNow)){
            transport.Invalidate(destination);return false;
        }
        return transport.Transfer(before,sourceNow,destination,destinationNow);
    }
    size_t Active()const noexcept {size_t active=0;for(const auto& pending:pending_)active+=pending.ticket!=0;return active;}
};

// Same-thread before/after API for the existing generic camera-copy hook.
// caller is its actual native return address, used only by bounded diagnostics.
uint64_t BeginEditorNativeCameraCopy(uintptr_t source)noexcept;
bool CompleteEditorNativeCameraCopy(uint64_t ticket,uintptr_t destination,uintptr_t caller)noexcept;
}

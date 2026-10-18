#include "MenuPreview/EditorCameraTransport.hpp"
#include <cstdio>
using namespace cvr::menu_preview;
static int failures{};
static void Check(bool value,const char* label){if(!value){std::printf("FAIL %s\n",label);++failures;}}
static std::shared_ptr<const CameraApplication> App(unsigned eye,uint64_t seq){
    auto value=std::make_shared<CameraApplication>();
    value->source={uintptr_t(0x10000+eye*0x1000),100+eye,eye,NativePreviewMode::NativeEditorDual,5};
    value->frame={{1,2,0x10000,100},20,eye,seq,true};
    int32_t position[3]{int32_t(eye?40:-40),30,50};float rotation[4]{0,0,0,1};
    std::memcpy(value->eyeDescriptor.data(),rotation,16);std::memcpy(value->eyeDescriptor.data()+16,position,12);
    const float projection[3]{90,1.25f,.01f};std::memcpy(value->eyeDescriptor.data()+0x38,projection,12);
    return value;
}
int main(){
    EditorCameraTransport store;const auto left=App(0,1),right=App(1,2);
    const auto lf=EditorSetupFingerprint(left->eyeDescriptor),rf=EditorSetupFingerprint(right->eyeDescriptor);
    Check(store.Publish(0x1000,lf,left)&&store.Publish(0x2000,rf,right),"independent native views publish");
    const auto before=store.Capture(0x1000,lf);
    Check(!store.Capture(0x3000,lf),"identical numeric pose at unregistered address is not ancestry");
    Check(store.Transfer(before,lf,0x1100,lf)&&store.Capture(0x1100,lf).payload==left,"clone carries exact left application");
    const auto rbefore=store.Capture(0x2000,rf);
    Check(store.Transfer(rbefore,rf,0x2100,rf)&&store.Capture(0x2100,rf).payload==right,"clone carries exact right application");
    Check(!store.Transfer(before,lf,0x1200,rf)&&!store.Capture(0x1200,rf),"different eye destination rejects");
    Check(store.Publish(0x1000,lf,App(0,3))&&!store.Transfer(before,lf,0x1300,lf),"same bytes reused source revokes captured generation");
    store.Invalidate(0x1100);Check(!store.Capture(0x1100,lf),"default setup invalidates old editor ancestry");
    const auto unrelatedOverwrite=store.Capture(0x2100,rf);
    store.Invalidate(0x2100); // DescriptorBuild entry, including unowned native writes.
    Check(!store.Capture(0x2100,rf)&&!store.Transfer(unrelatedOverwrite,rf,0x2200,rf),"unowned identical native overwrite revokes application and outstanding copies");
    auto descriptor=left->eyeDescriptor;descriptor[0x28]=1;
    Check(MatchesEditorRenderedDescriptor(*left,descriptor),"unowned auxiliary reconstruction allowed");
    descriptor[0x38]^=1;Check(!MatchesEditorRenderedDescriptor(*left,descriptor),"changed camera projection rejected");
    auto bad=std::make_shared<CameraApplication>(*left);bad->frame.eye=1;
    Check(!store.Publish(0x1400,lf,bad),"frame/source eye mismatch rejects");
    auto prepared=std::make_shared<CameraApplication>(*left);prepared->baseDescriptor=prepared->eyeDescriptor;
    Check(IsEditorNativePreparation(*prepared),"unmapped native receipt is preparation only");
    const auto preparationFingerprint=EditorSetupFingerprint(prepared->eyeDescriptor);
    Check(store.Publish(0x3000,preparationFingerprint,prepared),"preparation has private address ancestry");
    const auto preparedCopy=store.Capture(0x3000,preparationFingerprint);
    Check(store.Transfer(preparedCopy,preparationFingerprint,0x3100,preparationFingerprint),"original preparation reaches registration");
    auto applied=std::make_shared<CameraApplication>(*prepared);applied->projection.valid=true;applied->projection.origin=9;
    applied->frame.nativeFrame=prepared->frame.nativeFrame+1; // observed renderer phases, not a runtime offset policy
    int32_t mappedX=900;std::memcpy(applied->eyeDescriptor.data()+16,&mappedX,4);
    const auto mappedFingerprint=EditorSetupFingerprint(applied->eyeDescriptor);
    Check(!IsEditorNativePreparation(*applied),"tracked projection cannot receive a second head/IPD application");
    Check(store.Publish(0x3100,mappedFingerprint,applied)&&!store.Capture(0x3100,preparationFingerprint),"registered application replaces original destination proof");
    Check(store.Capture(0x3000,preparationFingerprint).payload==prepared,"upstream original cache is unchanged by registered mapping");
    Check(!store.Transfer(preparedCopy,preparationFingerprint,0x3200,mappedFingerprint),"copy cannot relabel mapped bytes as original preparation");
    auto staleMarker=*prepared;staleMarker.nativeContext=0x7000;
    Check(!IsEditorNativePreparation(staleMarker),"a native context cannot be a preparation marker");
    staleMarker=*prepared;staleMarker.baseDescriptor[0]^=1;
    Check(!IsEditorNativePreparation(staleMarker),"an already modified descriptor cannot claim original preparation");
    // Exercise the production age predicate with real address ancestry and
    // registered source state: an original native packet may remain cached
    // while a later fresh Lease supplies the registered tracked application.
    constexpr uint64_t now=2000;Metadata common{};common.screen=2;common.screenEpoch=1;common.setupID=2;
    common.controllerIdentity=10;common.entityIdentity=20;common.entityID=21;common.componentIdentity=left->source.component;
    common.virtualCameraNameHash=left->source.cameraKey;common.sceneNameHash=50;common.cameraRefHash=51;common.slotNameHash=52;
    common.publicationSerial=1;common.publishedAtMs=1000;
    Check(ConfigureEditorNativeApplications(common,5,left->source,right->source),"native preparation owner is registered");
    auto cached=std::make_shared<CameraApplication>(*prepared);cached->metadata=common;
    Check(store.Publish(0x5000,preparationFingerprint,cached),"original baseline cache publishes exact ancestry");
    auto current=common;current.publicationSerial=2;current.publishedAtMs=now;
    Check(ConfigureEditorNativeApplications(current,5,left->source,right->source)&&FreshPublication(current.publishedAtMs,now),"same native owner refreshes the current Lease without rebuilding original cache");
    const auto capture=[&](uintptr_t address,const cvr::camera::PoseFingerprint& fingerprint,uint64_t when){
        auto receipt=store.Capture(address,fingerprint);
        if(!receipt||!EditorNativeSourceRegistered(receipt.payload->metadata,receipt.payload->source)||
           !EditorNativeSetupCaptureAgeAllowed(*receipt.payload,when))return EditorCameraTransport::Receipt{};
        return receipt;
    };
    const auto oldCache=capture(0x5000,preparationFingerprint,now);
    Check(bool(oldCache),"expired original preparation remains admissible for the current registered owner");
    Check(oldCache&&store.Transfer(oldCache,preparationFingerprint,0x5100,preparationFingerprint),"old original cache copies to a later native registration");
    auto tracked=std::make_shared<CameraApplication>(*cached);tracked->metadata=current;
    tracked->nativeContext=0x9000;tracked->projection.valid=true;tracked->projection.origin=7;
    tracked->frame.nativeFrame=300;tracked->frame.sourceSequence=99;tracked->localPoseRevision=99;
    std::memcpy(tracked->eyeDescriptor.data()+16,&mappedX,4);const auto trackedFingerprint=EditorSetupFingerprint(tracked->eyeDescriptor);
    Check(oldCache&&SameEditorApplicationScope(oldCache.payload->metadata,current)&&
        store.Publish(0x5100,trackedFingerprint,tracked)&&capture(0x5100,trackedFingerprint,now).payload==tracked,
        "fresh current registration creates a distinct fresh tracked receipt from the old original baseline");
    Check(bool(capture(0x5100,trackedFingerprint,now+500))&&!capture(0x5100,trackedFingerprint,now+501),"tracked projection keeps the exact 500ms freshness limit");
    auto missing=*cached;missing.metadata.publishedAtMs=0;auto future=*cached;future.metadata.publishedAtMs=now+1;
    Check(!EditorNativeSetupCaptureAgeAllowed(missing,now)&&!EditorNativeSetupCaptureAgeAllowed(future,now),"original preparation still requires a known nonfuture timestamp");
    missing=*tracked;missing.metadata.publishedAtMs=0;future=*tracked;future.metadata.publishedAtMs=now+1;
    Check(!EditorNativeSetupCaptureAgeAllowed(missing,now)&&!EditorNativeSetupCaptureAgeAllowed(future,now),"tracked receipt rejects missing and future timestamps");
    auto changed=preparationFingerprint;++changed.position[0];
    Check(!capture(0x5000,changed,now),"extended preparation lifetime never bypasses exact fingerprint");
    auto nextLeft=left->source,nextRight=right->source;nextLeft.bindingGeneration=nextRight.bindingGeneration=6;
    Check(ConfigureEditorNativeApplications(current,6,nextLeft,nextRight)&&!capture(0x5000,preparationFingerprint,now),"a changed binding rejects the cached original source");
    Check(bool(store.Capture(0x5000,preparationFingerprint)),"binding rejection does not invent an address overwrite");
    store.Invalidate(0x5000);Check(!store.Capture(0x5000,preparationFingerprint)&&
        !store.Transfer(oldCache,preparationFingerprint,0x5200,preparationFingerprint),"source invalidation still revokes retained cache and outstanding copies");
    ClearCameraApplication();Check(!capture(0x5100,trackedFingerprint,now),"authoritative registration clear still rejects tracked ancestry");
    std::printf("editor camera transport failures=%d\n",failures);return failures?1:0;
}

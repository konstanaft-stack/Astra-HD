#include "Utils/DebugGate.hpp"
#include "Camera/PoseIdentity.hpp"
#include "Camera/MainEyeWriteDiagnostics.hpp"
#include "Camera/MainEyeTransportDiagnostics.hpp"
#include "Camera/MainEyeCameraRoute.hpp"
#include "Camera/CameraState.hpp"
#include "Utils/MemorySafe.hpp"
#include "Framegen/MainEyePoseSample.hpp"
#include <atomic>
#include <cstring>
#include <vector>

extern "C" {
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_MainEyeTransportStatusSeq{0};
__declspec(dllexport) cvr::camera::MainEyeTransportTelemetry CyberpunkVR_MainEyeTransportStatus{};
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_MainEyeWriteStatusSeq{0};
__declspec(dllexport) cvr::camera::MainEyeWriteTelemetry CyberpunkVR_MainEyeWriteStatus{};
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_MainEyePoseStatusSeq{0};
__declspec(dllexport) cvr::framegen::MainEyePoseTelemetry CyberpunkVR_MainEyePoseStatus{};
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_PoseIdComponent[2]{};
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_PoseIdCopied{0};
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_PoseIdCopyChanged{0};
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_PoseIdFinal[2]{};
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_PoseIdFinalMiss[2]{};
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_PoseIdVrikSource{0};
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_PoseIdVrikInputChanged{0};
}
namespace cvr::camera {
namespace {
PoseAddressLedger<PoseIdentity> s_addresses;
std::atomic<uint64_t> s_poseId{0},s_writeId{0};
thread_local PoseIdentity t_nodePose{};
std::mutex s_mainEyeSourceMutex;
framegen::MainEyeSourceLatch s_mainEyeSource;
MainEyeCameraRouteGate s_mainEyeRoute;
struct MainEyePoseValue {OpenXRHeadPose head{};roomscale::Vec2 consumed{};uint64_t poseId{};};
std::mutex s_mainEyePoseMutex;
framegen::MainEyePoseSample<MainEyePoseValue> s_mainEyePoseSamples;
std::mutex s_mainEyeTelemetryMutex;
template<class Change> void PoseTelemetry(Change&& change){
    std::lock_guard lock(s_mainEyeTelemetryMutex);
    CyberpunkVR_MainEyePoseStatusSeq.fetch_add(1,std::memory_order_acq_rel);
    change(CyberpunkVR_MainEyePoseStatus);
    CyberpunkVR_MainEyePoseStatusSeq.fetch_add(1,std::memory_order_release);
}
template<class Change> void TransportTelemetry(Change&& change){
    std::lock_guard lock(s_mainEyeTelemetryMutex);
    CyberpunkVR_MainEyeTransportStatusSeq.fetch_add(1,std::memory_order_acq_rel);
    change(CyberpunkVR_MainEyeTransportStatus);
    CyberpunkVR_MainEyeTransportStatusSeq.fetch_add(1,std::memory_order_release);
}
bool Fingerprint(uintptr_t address,PoseFingerprint* out) {
    if(address<0x10000 || !out)return false;
    __try {
        std::memcpy(out->position.data(),reinterpret_cast<const void*>(address),12);
        std::memcpy(out->rotation.data(),reinterpret_cast<const void*>(address+16),16);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}
uint64_t RequestMainEyeSource(bool enabled) {
    std::lock_guard lock(s_mainEyeSourceMutex);
    if(!enabled)s_mainEyeRoute.Reset();
    return s_mainEyeSource.Request(enabled);
}
void RecordMainEyeWrite(const MainEyeWriteObservation& value){
    std::lock_guard lock(s_mainEyeTelemetryMutex);auto& d=CyberpunkVR_MainEyeWriteStatus;
    CyberpunkVR_MainEyeWriteStatusSeq.fetch_add(1,std::memory_order_acq_rel);
    ++d.calls;d.writeBits=value.bits;d.sourceId=value.sourceId;d.writtenPoseId=value.poseId;
    d.requestedTimeNs=value.requestedTimeNs;d.writtenTimeNs=value.writtenTimeNs;
    if(value.bits&WritePublisherCalled)++d.publishCalls;
    if(value.bits&WritePublisherHasSource)++d.sourceCalls;
    if(!(value.bits&WriteOrientationWritten))++d.orientationMissing;
    if(!(value.bits&WriteTranslationLabelKnown))++d.translationMissing;
    if(!(value.bits&WritePoseIdPresent))++d.poseIdMissing;
    if(!(value.bits&WriteUseSource))++d.sourceMismatch;
    if(!(value.bits&WritePositionWritten))++d.positionFailed;
    CyberpunkVR_MainEyeWriteStatusSeq.fetch_add(1,std::memory_order_release);
}
void RecordMainEyeLocate(uint32_t bits){
    std::lock_guard lock(s_mainEyeTelemetryMutex);auto& d=CyberpunkVR_MainEyeWriteStatus;
    CyberpunkVR_MainEyeWriteStatusSeq.fetch_add(1,std::memory_order_acq_rel);
    ++d.locateCalls;d.locateBits=bits;if(!(bits&LocateSourceStamped))++d.locateMissingSource;
    CyberpunkVR_MainEyeWriteStatusSeq.fetch_add(1,std::memory_order_release);
}
bool PublishMainEyeSceneFrame(uint64_t revision,const framegen::MainEyeSceneFrame& frame,int64_t sourceId,
    framegen::AerV2Eye eye,const framegen::VrcamOffProof& off,const framegen::MainEyeClockSnapshot& snapshot) {
    // snapshot is a value copied under the clock owner's existing lock. No
    // game/camera thread reaches into or modifies that live clock instance.
    std::lock_guard lock(s_mainEyeSourceMutex);return s_mainEyeSource.OnSceneFrame(revision,frame,sourceId,eye,off,snapshot);
}
void InvalidateMainEyeSource() {std::lock_guard lock(s_mainEyeSourceMutex);s_mainEyeSource.Invalidate();}
bool MainEyeSourceRequested() {std::lock_guard lock(s_mainEyeSourceMutex);return s_mainEyeSource.Requested();}
std::optional<framegen::MainEyeSourceReceipt> AcquireMainEyeSourceForWrite(uint64_t origin) {
    std::lock_guard lock(s_mainEyeSourceMutex);return s_mainEyeSource.ForMainWrite(origin);
}
bool ValidateMainEyeSourceReceipt(const framegen::MainEyeSourceReceipt& receipt) {
    std::lock_guard lock(s_mainEyeSourceMutex);return s_mainEyeSource.Usable(receipt);
}
bool AdmitMainEyeSelectedCamera(const PoseReceipt& selected,uintptr_t selectedSetup){
    const auto origin=OpenXRManager::Get().GetTrackingOriginSerial();
    std::lock_guard lock(s_mainEyeSourceMutex);
    const auto current=s_mainEyeSource.ForMainWrite(origin);
    const auto latchBefore=s_mainEyeSource.LatchRevision();
    const auto& source=selected.payload.mainEyeSource;
    const bool known=selected && selected.payload.mainEyeSource && s_mainEyeSource.Usable(selected.payload.mainEyeSource);
    const auto route=known ? (selected.payload.externalView ? MainEyeCameraRoute::Scene : MainEyeCameraRoute::Component)
                           : MainEyeCameraRoute::Unknown;
    const bool admitted=AdmitSelectedMainEyeRoute(s_mainEyeRoute,s_mainEyeSource,route);
    const auto latchAfter=s_mainEyeSource.LatchRevision();
    TransportTelemetry([&](auto& d){
        ++d.mainReadCalls;if(!selected)++d.mainReadMissing;
        if(selected && !known)++d.mainReadUnusable;
        if(admitted)++d.mainReadAdmitted;if(latchBefore!=latchAfter)++d.routeInvalidations;
        d.selectedSetup=selectedSetup;d.selectedSourceId=source?source.sourceId:-1;
        d.currentSourceId=current?current->sourceId:-1;d.selectedLatch=source.latchRevision;
        d.currentLatch=latchBefore;d.latchAfter=latchAfter;d.selectedPoseId=selected.payload.poseId;
        d.selectedComponent=selected.payload.component;d.route=uint32_t(route);d.bits=0;
        auto bit=[&](MainEyeTransportBit mask,bool condition){if(condition)d.bits|=mask;};
        bit(TransportSelectedPresent,bool(selected));bit(TransportSourcePresent,bool(source));
        bit(TransportRequested,s_mainEyeSource.Requested());bit(TransportCurrentPresent,bool(current));
        if(source && current){
            bit(TransportRequestMatches,source.requestRevision==current->requestRevision);
            bit(TransportLatchMatches,source.latchRevision==latchBefore);
            bit(TransportEpochsMatch,source.epochs==current->epochs);
            bit(TransportClockMatches,source.clockGeneration==current->clockGeneration);
            bit(TransportOrdinalWithin,source.sceneOrdinal<=current->sceneOrdinal);
            bit(TransportSourceWithin,source.sourceId<=current->sourceId);
        }
        bit(TransportUsable,known);bit(TransportAdmitted,admitted);bit(TransportInvalidated,latchBefore!=latchAfter);
        bit(TransportExternal,selected && selected.payload.externalView);bit(TransportViewMain,selected && selected.payload.view==1);
    });
    return admitted;
}
void RecordMainEyeTransportJoin(MainEyeTransportJoin join,uintptr_t address,bool sourceTransferred){
    TransportTelemetry([&](auto& d){switch(join){
        case MainEyeTransportJoin::Serialize:++d.serializeCalls;if(sourceTransferred)++d.serializeLabelled;d.serializedSetup=address;break;
        case MainEyeTransportJoin::Blend:++d.blendCalls;if(sourceTransferred)++d.blendLabelled;break;
        case MainEyeTransportJoin::MainRead:if(sourceTransferred)++d.mainReadTransferred;break;
        case MainEyeTransportJoin::Descriptor:++d.descriptorCalls;if(sourceTransferred)++d.descriptorTransferred;break;
    }});
}
std::optional<framegen::MainEyeSourceFaultScope> CaptureMainEyeSourceFaultScope(uint64_t worldEpoch,uint64_t ordinal) {
    std::lock_guard lock(s_mainEyeSourceMutex);return s_mainEyeSource.FaultScope(worldEpoch,ordinal);
}
bool RevokeMainEyeSourceFault(const framegen::MainEyeSourceFaultScope& scope) {
    std::lock_guard lock(s_mainEyeSourceMutex);return s_mainEyeSource.RevokeFault(scope);
}
bool ApplyReferenceCameraOptics(OpenXRHeadPose& head) {
    if(!head.valid)return false;
    if(head.referenceOptics.valid)return true;
    XrFovf raw[2]{};
    if(!OpenXRManager::Get().GetCurrentEyeFovs(raw))return false;
    const auto optics=cvr::ppd::BuildReferenceOptics({
        cvr::ppd::PpdRenderedFov{raw[0].angleLeft,raw[0].angleRight,raw[0].angleUp,raw[0].angleDown},
        cvr::ppd::PpdRenderedFov{raw[1].angleLeft,raw[1].angleRight,raw[1].angleUp,raw[1].angleDown}});
    if(!optics.valid)return false;
    const float half=optics.pitch*.5f,s=std::sin(half),c=std::cos(half);
    const auto q=head;
    head.oriX=q.oriW*s+q.oriX*c;head.oriY=q.oriY*c+q.oriZ*s;
    head.oriZ=q.oriZ*c-q.oriY*s;head.oriW=q.oriW*c-q.oriX*s;
    head.referenceOptics=optics;return true;
}
bool AcquireMainEyeSourcePose(const framegen::MainEyeSourceReceipt& receipt,OpenXRHeadPose* head,
    roomscale::Vec2* consumed,uint64_t* poseId) {
    if(!head || !consumed || !poseId){
        PoseTelemetry([&](auto& d){++d.attempts;++d.invalidSample;d.lastSourceId=receipt.sourceId;d.acquireCode=uint32_t(framegen::MainEyePoseCode::InvalidSample);});
        return false;
    }
    auto& xr=OpenXRManager::Get();
    const auto current=[&](const auto& r){
        if(!xr.IsSessionRunning() || xr.ExternalPoseResetPending() || xr.GetTrackingOriginSerial()!=r.epochs.hmdWorld)return false;
        const auto active=AcquireMainEyeSourceForWrite(r.epochs.hmdWorld);return active && *active==r;
    };
    std::lock_guard lock(s_mainEyePoseMutex);
    // Diagnostic-only sampling of the existing runtime QPC mapping. Unknown
    // stays unknown; this is not a substitute pose time or a new prediction.
    const auto actualTime=xr.SampleCameraApplicationTime(receipt.epochs.hmdWorld);
    framegen::HeadPoseQueryDiagnostics query;
    framegen::MainEyePoseCode code=framegen::MainEyePoseCode::None;
    OpenXRHeadPose located{};
    const auto result=s_mainEyePoseSamples.Acquire(receipt,current,[&](int64_t target,MainEyePoseValue& value){
        // Preserve the existing native body's consumed roomscale ledger and
        // posture data. Only the rendered head pose is queried at source time.
        OpenXRHeadPose bodyReference{};
        if(!xr.AcquireCameraPoseFrame(&bodyReference,&value.consumed) || !bodyReference.valid ||
           bodyReference.originSerial!=receipt.epochs.hmdWorld){query.code=framegen::HeadPoseQueryCode::BodyUnavailable;return false;}
        const bool ok=xr.LocateHeadPoseAt(target,&value.head,true,&query);
        located=value.head;
        if(!ok || !value.head.valid)return false;
        // Original2DCBB0: queried HEAD * pitch correction, before game/world
        // anchoring. Never rotate the physical IPD by an invented per-eye yaw.
        if(!ApplyReferenceCameraOptics(value.head))return false;
        value.head.bodyBend=bodyReference.bodyBend;
        value.poseId=NextPoseIdentity();return true;
    },&code);
    PoseTelemetry([&](auto& d){
        ++d.attempts;d.lastSourceId=receipt.sourceId;d.sceneOrdinal=receipt.sceneOrdinal;
        d.sourceClockGeneration=receipt.clockGeneration;d.requestedTimeNs=receipt.requestedPoseTimeNs;
        d.acquireCode=uint32_t(code);d.queryCode=uint32_t(query.code);d.xrResult=query.xrResult;d.locationFlags=query.locationFlags;
        d.actualTimeKnown=actualTime?1u:0u;d.actualTimeNs=actualTime?actualTime->xrTime:0;d.qpc=actualTime?actualTime->qpc:0;
        d.locatedTimeNs=result?result->head.locatedTime:located.locatedTime;
        d.originSerial=result?result->head.originSerial:located.originSerial;d.poseId=result?result->poseId:0;
        if(result)++d.success;
        using Code=framegen::MainEyePoseCode;
        if(code==Code::CacheHit)++d.cacheHits;
        if(code==Code::LocateFailed)++d.locateFailed;
        if(code==Code::InvalidReceipt || code==Code::OwnerRejected)++d.receiptRejected;
        if(code==Code::OwnerChanged)++d.receiptChanged;
        if(code==Code::StaleScene)++d.staleScene;
        if(code==Code::InvalidSample)++d.invalidSample;
        if(query.code==framegen::HeadPoseQueryCode::BodyUnavailable)++d.bodyUnavailable;
    });
    if(!result)return false;
    *head=result->head;*consumed=result->consumed;*poseId=result->poseId;return true;
}
bool ReadMainEyeSourcePose(const framegen::MainEyeSourceReceipt& receipt,OpenXRHeadPose* head,
    roomscale::Vec2* consumed,uint64_t* poseId) {
    if(!head || !consumed || !poseId)return false;
    auto& xr=OpenXRManager::Get();
    std::lock_guard lock(s_mainEyePoseMutex);
    framegen::MainEyePoseCode code=framegen::MainEyePoseCode::None;
    const auto result=s_mainEyePoseSamples.Read(receipt,[&](const auto& r){
        return xr.IsSessionRunning() && !xr.ExternalPoseResetPending() &&
            xr.GetTrackingOriginSerial()==r.epochs.hmdWorld && ValidateMainEyeSourceReceipt(r);
    },&code);
    PoseTelemetry([&](auto& d){++d.readAttempts;d.lastReadSourceId=receipt.sourceId;d.readCode=uint32_t(code);if(!result)++d.readFailed;});
    if(!result)return false;
    *head=result->head;*consumed=result->consumed;*poseId=result->poseId;return true;
}
uint64_t NextPoseIdentity() {
    // Native packets already use bit63; XR-latched samples use neither bit.
    // Keep legacy, separately located samples in their own identity domain.
    return (s_poseId.fetch_add(1,std::memory_order_relaxed)+1) | (uint64_t{1}<<62);
}
void InvalidatePoseAddress(uintptr_t address) { s_addresses.Invalidate(address); }
PoseReceipt CapturePoseAddress(uintptr_t address) {
    PoseFingerprint fingerprint{};
    return Fingerprint(address,&fingerprint) ? s_addresses.Capture(address,fingerprint) : PoseReceipt{};
}
bool TransferPoseAddress(const PoseReceipt& source,uintptr_t destination) {
    PoseFingerprint sourceNow{},destinationNow{};
    if(!source || !Fingerprint(source.source,&sourceNow) || !Fingerprint(destination,&destinationNow)) {
        s_addresses.Invalidate(destination);return false;
    }
    auto carried=source;
    if(!carried.payload.appliedEyeReference.MatchesWrittenFingerprint(sourceNow) ||
       !carried.payload.appliedEyeReference.MatchesWrittenFingerprint(destinationNow))
        carried.payload.appliedEyeReference={};
    const bool copied=s_addresses.Transfer(carried,sourceNow,destination,destinationNow);
    CVR_DIAGNOSTIC((copied ? CyberpunkVR_PoseIdCopied : CyberpunkVR_PoseIdCopyChanged).fetch_add(1,std::memory_order_relaxed));
    return copied;
}
void PublishComponentPose(uintptr_t component,uint32_t view,uint64_t poseId,const OpenXRHeadPose& head,
    const framegen::MainEyeSourceReceipt& mainSource,const AppliedEyeReference& appliedEyeReference) {
    PublishCameraSetupPose(component+0xE0,component,view,poseId,head,false,mainSource,appliedEyeReference);
}
void PublishCameraSetupPose(uintptr_t address,uintptr_t component,uint32_t view,uint64_t poseId,const OpenXRHeadPose& head,bool externalView,
    const framegen::MainEyeSourceReceipt& mainSource,const AppliedEyeReference& appliedEyeReference) {
    PoseIdentity value{};
    PoseFingerprint fingerprint{};
    if(!poseId || view<1 || view>2 || !Fingerprint(address,&fingerprint) ||
       !OpenXRManager::Get().ResolveRenderedPose(head,&value.localHead)) {
        if(mainSource)PoseTelemetry([&](auto& d){++d.publishRejected;d.lastPublishSourceId=mainSource.sourceId;});
        s_addresses.Invalidate(address);return;
    }
    if(mainSource && (view!=1 || !mainSource.MatchesWrittenPose(head.originSerial,head.locatedTime) ||
       !ValidateMainEyeSourceReceipt(mainSource))) {
        PoseTelemetry([&](auto& d){++d.publishRejected;d.lastPublishSourceId=mainSource.sourceId;});
        s_addresses.Invalidate(address);return;
    }
    value.poseId=poseId;value.writeId=s_writeId.fetch_add(1,std::memory_order_relaxed)+1;
    value.component=component;value.view=view;value.head=head;
    value.externalView=externalView;
    value.mainEyeSource=mainSource;
    if(view==1 && mainSource && appliedEyeReference.MatchesWrittenFingerprint(fingerprint) &&
       appliedEyeReference.route==(externalView ? AppliedEyeRoute::Scene:AppliedEyeRoute::Component))
        value.appliedEyeReference=appliedEyeReference;
    value.posePredictionNs=head.locatedTime;
    if(const auto time=OpenXRManager::Get().SampleCameraApplicationTime(head.originSerial)) {
        value.cameraApplicationQpc=time->qpc;value.contentTimeNs=time->xrTime;value.clockGeneration=time->clockGeneration;
    }
    s_addresses.Publish(address,fingerprint,value);
    if(mainSource)TransportTelemetry([&](auto& d){d.publishedSetup=address;});
    if(mainSource)PoseTelemetry([&](auto& d){
        ++d.published;d.lastPublishSourceId=mainSource.sourceId;d.writeId=value.writeId;d.captureClockGeneration=value.clockGeneration;
        if(!value.clockGeneration)++d.publishClockMissing;
    });
    CVR_DIAGNOSTIC(CyberpunkVR_PoseIdComponent[view-1].fetch_add(1,std::memory_order_relaxed));
}
void PublishSerializedPose(uintptr_t destination,const PoseReceipt& source) {
    // Serialisation on the component path preserves these fields. A camera
    // override changing them must supply its own label, not inherit this one.
    PoseFingerprint output{};
    if(!source || !Fingerprint(destination,&output) || output!=source.fingerprint) {
        s_addresses.Invalidate(destination);return;
    }
    TransferPoseAddress(source,destination);
}
bool ReadCameraPoseIdentity(uintptr_t camera,uint32_t view,PoseIdentity* out) {
    if(!out || view<1 || view>2 || OpenXRManager::Get().ExternalPoseResetPending())return false;
    const auto record=CapturePoseAddress(camera);
    if(!record || record.payload.view!=view ||
       record.payload.head.originSerial!=OpenXRManager::Get().GetTrackingOriginSerial())return false;
    if(record.payload.mainEyeSource && !ValidateMainEyeSourceReceipt(record.payload.mainEyeSource))return false;
    *out=record.payload;return true;
}
bool PublishBlendedPose(uintptr_t destination,std::span<const WeightedPoseReceipt> inputs) {
    PoseFingerprint output{};
    if(!Fingerprint(destination,&output)) { s_addresses.Invalidate(destination);return false; }
    std::vector<PoseAddressLedger<PoseIdentity>::BlendInput> verified;
    verified.reserve(inputs.size());
    for(const auto& input:inputs) {
        PoseFingerprint current{};
        if(input.weight!=0 && (!input.source || input.source.payload.view!=1 ||
           !Fingerprint(input.source.source,&current))) {
            s_addresses.Invalidate(destination);return false;
        }
        verified.push_back({input.source,current,input.weight});
    }
    const bool copied=s_addresses.TransferBlend(verified,destination,output,[](const PoseIdentity& a,const PoseIdentity& b) {
        return a.view==1 && b.view==1 && a.poseId==b.poseId &&
               a.head.originSerial==b.head.originSerial && a.mainEyeSource==b.mainEyeSource &&
               std::memcmp(&a.localHead,&b.localHead,sizeof(a.localHead))==0;
    });
    if(copied) {
        const bool external=std::any_of(inputs.begin(),inputs.end(),[](const auto& i){return i.weight>0 && i.source.payload.externalView;});
        const auto receipt=s_addresses.Capture(destination,output);
        if(receipt) {
            auto value=receipt.payload;
            // Common head/source provenance alone does not prove the centre of
            // a native camera blend. Keep raw eye evidence only for an exact
            // unchanged input (or multiple identical inputs); preserve the old
            // AER head label even when the new SR-only evidence is unavailable.
            auto eyeReference=value.appliedEyeReference;
            if(!eyeReference.MatchesWrittenFingerprint(output) ||
               std::any_of(inputs.begin(),inputs.end(),[&](const auto& i) {
                   return i.weight!=0 && (i.source.fingerprint!=output ||
                       i.source.payload.appliedEyeReference!=eyeReference);
               }))eyeReference={};
            const bool mixedClock=std::any_of(inputs.begin(),inputs.end(),[&](const auto& i) {
                return i.weight!=0 && (i.source.payload.contentTimeNs!=value.contentTimeNs ||
                    i.source.payload.posePredictionNs!=value.posePredictionNs ||
                    i.source.payload.cameraApplicationQpc!=value.cameraApplicationQpc ||
                    i.source.payload.clockGeneration!=value.clockGeneration);
            });
            if(value.externalView!=external || mixedClock || value.appliedEyeReference!=eyeReference) {
                value.externalView=external;
                value.appliedEyeReference=eyeReference;
                if(mixedClock) {value.contentTimeNs=value.posePredictionNs=value.cameraApplicationQpc=0;value.clockGeneration=0;}
                s_addresses.Publish(destination,output,value);
            }
        }
    }
    return copied;
}
bool ReadContextPoseIdentity(uintptr_t context,PoseIdentity* out) {
    return context && (ReadCameraPoseIdentity(context+0x70,1,out) || ReadCameraPoseIdentity(context+0x70,2,out));
}
PoseIdentity CurrentNodePoseIdentity() { return t_nodePose; }
PoseIdentity SetNodePoseIdentity(const PoseIdentity& pose) { const auto previous=t_nodePose;t_nodePose=pose;return previous; }
}

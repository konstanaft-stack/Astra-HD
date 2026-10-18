#include "Utils/DebugGate.hpp"
#include "Camera/PoseIdentity.hpp"
#include "Camera/MainEyeTransportDiagnostics.hpp"
#include "Camera/ExternalViewMath.hpp"
#include "Camera/ExternalView.hpp"
#include "Camera/PlayerCameraState.hpp"
#include "Camera/ExternalViewLease.hpp"
#include "Camera/CameraState.hpp"
#include "Stereo/StereoInternal.hpp"
#include "Hooks/Hook.hpp"
#include "Hooks/CameraPoseSites.hpp"
#include "Utils/MemorySafe.hpp"
#include "Utils/WeakObjectSlot.hpp"
#include "Core/VrCoreShared.hpp"
#include "MenuPreview/Producer.hpp"
#include "MenuPreview/CameraApplication.hpp"
#include "MenuPreview/EditorNativeRenderPreparation.hpp"
#include "MenuPreview/ProducerRoute.hpp"
#include "MenuPreview/LocalComponentPose.hpp"
#include "MenuPreview/UiConsumer.hpp"
#include "MenuPreview/SpatialEditor.hpp"
#include "MenuPreview/EditorCameraTransport.hpp"
#include "MenuPreview/RegisteredEditorCamera.hpp"
#include "Framegen/Inputs.hpp"
#include <RED4ext/Scripting/Natives/entIComponent.hpp>
#include <MinHook.h>
#include <intrin.h>
#include <cstring>
#include <cstdio>
#include <new>
#include <type_traits>
#include <vector>
#include <array>

struct PoseBlendDebug {
    uint64_t calls{},labelled{},rejected{};
    uintptr_t director{};
    uint32_t count{},accepted{};
    float weights[8]{};
    uint64_t poseIds[8]{};
    uintptr_t components[8]{};
    cvr::camera::PoseFingerprint inputs[8]{},output{};
};
struct ExternalCameraDebug {
    uint64_t writes{},stampUs{},poseId{},origin{},component{};
    int32_t position[3]{};
    float rotation[4]{},head[7]{},fov{};
};
static_assert(sizeof(ExternalCameraDebug)==104);
extern "C" {
__declspec(dllexport) PoseBlendDebug CyberpunkVR_PoseBlendDebug{};
__declspec(dllexport) std::atomic<uint32_t> CyberpunkVR_PoseBlendDebugSeq{};
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_PoseMainReadMissing[2]{}; // blended / override setup
__declspec(dllexport) ExternalCameraDebug CyberpunkVR_ExternalCameraDebug[2]{};
__declspec(dllexport) std::atomic<uint32_t> CyberpunkVR_ExternalCameraDebugSeq[2]{};
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_ExternalSelectionStates[4]{}; // fresh / retained / inactive / unavailable
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_ExternalNotify[3]{}; // MAIN pushes / expired or replaced / disabled or detached
}

bool EnsureCameraPoseCopyHook();

namespace {
cvr::menu_preview::EditorCameraTransport s_editorCameraTransport;
using EditorSetupReceipt=cvr::menu_preview::EditorCameraTransport::Receipt;
enum class EditorRegistrationStep:uint8_t {None,CaptureConfigured,CaptureFingerprint,CaptureLedger,CaptureSource,CaptureAge,Before,Preparation,Destination,Key,Lease,LeaseSource,Source,Scope,Registered,NativeRead,Reconstruct,Compose,Projection,Overlay,Clock,Frame,Origin,Write,Fingerprint,Publish,LateReceipt,LatePayload,LateSource,LateScope,LateProjection,LateOrigin,LateClock,LateFrame,LateDescriptor,LatePublish,Accepted,Transfer,AfterClock,AfterFrame,AfterOrigin,AfterSource};
const char* EditorRegistrationStepName(EditorRegistrationStep step)noexcept{
    static constexpr const char* names[]{"none","capture-configured","capture-fingerprint","capture-ledger","capture-source","capture-age","before-receipt","preparation","destination","native-key","current-lease","lease-source","source","full-scope","registered-source","native-read","reconstruct","compose","projection","overlay","clock","frame","origin","write","fingerprint","publish","late-receipt","late-payload","late-source","late-scope","late-projection","late-origin","late-clock","late-frame","late-descriptor","late-publish","accepted","transfer","after-clock","after-frame","after-origin","after-source"};
    const auto index=size_t(step);return index<std::size(names)?names[index]:"unknown";
}
struct EditorApplicationDiagnostic {uintptr_t identity{},context{};uint64_t sequence{},key{},component{},binding{},origin{},controls{},published{};uint32_t frame{},eye{};bool projection{};};
EditorApplicationDiagnostic EditorApplicationSnapshot(const cvr::menu_preview::CameraApplication* app)noexcept{
    if(!app)return {};return {reinterpret_cast<uintptr_t>(app),app->nativeContext,app->frame.sourceSequence,app->source.cameraKey,app->source.component,app->source.bindingGeneration,app->projection.origin,app->editorControlRevision,app->metadata.publishedAtMs,app->frame.nativeFrame,app->frame.eye,app->projection.valid};
}
struct EditorRegistrationDiagnostic {
    EditorRegistrationStep failure=EditorRegistrationStep::None,captureFailure=EditorRegistrationStep::None;
    cvr::menu_preview::EditorNativeComposeFailure compose=cvr::menu_preview::EditorNativeComposeFailure::None;
    uint64_t evaluated{},passed{},key{},leasePublished{},origin{},afterOrigin{};uintptr_t address{},context{};uint32_t frame{},afterFrame{};
    EditorApplicationDiagnostic incoming{};bool nativeKnown{},descriptorKnown{},poseMatch{},opticsMatch{};
    int32_t nativePosition[3]{};uint32_t nativeQuaternion[4]{},nativeOptics[4]{},expectedOptics[3]{},actualOptics[3]{};double nativeQuaternionNorm{};
};
bool EditorRegistrationCheck(EditorRegistrationDiagnostic* diagnostic,EditorRegistrationStep step,bool passed)noexcept{
    if(diagnostic){const auto bit=uint64_t(1)<<uint8_t(step);diagnostic->evaluated|=bit;if(passed)diagnostic->passed|=bit;else diagnostic->failure=step;}return passed;
}
bool EditorCaptureCheck(EditorRegistrationDiagnostic* diagnostic,EditorRegistrationStep step,bool passed)noexcept{
    if(diagnostic&&!passed)diagnostic->captureFailure=step;return EditorRegistrationCheck(diagnostic,step,passed);
}
void NoteEditorNativeFields(EditorRegistrationDiagnostic& d,const std::array<uint8_t,0x48>& native)noexcept{
    d.nativeKnown=true;std::memcpy(d.nativePosition,native.data(),12);std::memcpy(d.nativeQuaternion,native.data()+0x10,16);
    float q[4]{};std::memcpy(q,native.data()+0x10,16);for(float value:q)d.nativeQuaternionNorm+=double(value)*value;
    for(unsigned i=0;i<4;++i){const size_t offsets[]{0x20,0x28,0x40,0x44};std::memcpy(&d.nativeOptics[i],native.data()+offsets[i],4);}
}
void NoteEditorRegistration(const char* phase,const cvr::menu_preview::Metadata& scope,const cvr::menu_preview::NativePreviewSource& source,
    const EditorRegistrationDiagnostic& d,bool beforeClear=false)noexcept{
    using namespace cvr::menu_preview;
    if(scope.screen!=2||!scope.screenEpoch||source.mode!=NativePreviewMode::NativeEditorDual||source.eye>1||!source.component||!source.cameraKey)return;
    const auto reason=d.failure==EditorRegistrationStep::Compose?64u+unsigned(d.compose):
        d.failure==EditorRegistrationStep::LateReceipt&&d.captureFailure!=EditorRegistrationStep::None?96u+unsigned(d.captureFailure):unsigned(d.failure);if(reason>=128)return;
    static std::array<std::atomic<uint64_t>,256> counts{};const auto count=counts[source.eye*128+reason].fetch_add(1,std::memory_order_relaxed)+1;
    if(count>4&&count!=128&&count!=1024)return;
    try{EditorApplicationDiagnostic current;bool currentRead=false;
        if(beforeClear){CameraApplication probe;probe.metadata=scope;probe.source=source;const auto actual=ReadCurrentApplicationForSource(probe);current=EditorApplicationSnapshot(actual.get());currentRead=true;}
        const auto now=PublicationNowMs();const auto& in=d.incoming;
        Log("[EditorNativeRegistration] phase=%s reason=%s composeReason=%s count=%llu epoch=%u setup=%u eye=%u key=%llx binding=%llu address=%llx context=%llx actualKey=%llx evaluated=%llx passed=%llx captureReason=%s leasePublished=%llu now=%llu frame=%u origin=%llu afterFrame=%u afterOrigin=%llu incoming=%llx incomingContext=%llx incomingFrame=%u incomingSource=%llu incomingEye=%u incomingKey=%llx incomingBinding=%llu incomingPublished=%llu incomingProjection=%u incomingOrigin=%llu incomingControls=%llu preClear=%u currentRead=%u current=%llx currentContext=%llx currentFrame=%u currentSource=%llu currentEye=%u currentKey=%llx currentBinding=%llu currentPublished=%llu currentProjection=%u currentOrigin=%llu; evaluated-mask distinguishes unread operands, diagnostics only\n",
            phase,EditorRegistrationStepName(d.failure),EditorNativeComposeFailureName(d.compose),count,scope.screenEpoch,scope.setupID,source.eye,source.cameraKey,source.bindingGeneration,d.address,d.context,d.key,d.evaluated,d.passed,EditorRegistrationStepName(d.captureFailure),d.leasePublished,now,d.frame,d.origin,d.afterFrame,d.afterOrigin,in.identity,in.context,in.frame,in.sequence,in.eye,in.key,in.binding,in.published,unsigned(in.projection),in.origin,in.controls,unsigned(beforeClear),unsigned(currentRead),current.identity,current.context,current.frame,current.sequence,current.eye,current.key,current.binding,current.published,unsigned(current.projection),current.origin);
        if(d.nativeKnown&&(d.failure==EditorRegistrationStep::Reconstruct||d.failure==EditorRegistrationStep::Overlay))Log("[EditorNativeRegistrationFields] phase=%s reason=%s eye=%u key=%llx context=%llx nativePosition=%d,%d,%d nativeQuaternion=%08X,%08X,%08X,%08X nativeQuaternionNorm=%.12g nativeFovBits=%08X nativeAspectBits=%08X nativeNearBits=%08X nativeFarBits=%08X; already-read original native bytes only\n",phase,EditorRegistrationStepName(d.failure),source.eye,source.cameraKey,d.context,d.nativePosition[0],d.nativePosition[1],d.nativePosition[2],d.nativeQuaternion[0],d.nativeQuaternion[1],d.nativeQuaternion[2],d.nativeQuaternion[3],d.nativeQuaternionNorm,d.nativeOptics[0],d.nativeOptics[1],d.nativeOptics[2],d.nativeOptics[3]);
        if(d.descriptorKnown)Log("[EditorNativeRegistrationFields] phase=late reason=%s eye=%u key=%llx context=%llx poseMatch=%u opticsMatch=%u expectedFovAspectNear=%08X,%08X,%08X actualFovAspectNear=%08X,%08X,%08X; already-read Descriptors only\n",EditorRegistrationStepName(d.failure),source.eye,source.cameraKey,d.context,unsigned(d.poseMatch),unsigned(d.opticsMatch),d.expectedOptics[0],d.expectedOptics[1],d.expectedOptics[2],d.actualOptics[0],d.actualOptics[1],d.actualOptics[2]);
    }catch(...){} // No diagnostic failure can change admission or the existing clear.
}
bool RetainEditorRegisteredPreparation(const EditorSetupReceipt&,uintptr_t);
bool ReadPreviewBytes(uintptr_t,void*,size_t);
bool ReadEditorSetupFingerprint(uintptr_t address,cvr::camera::PoseFingerprint& output)noexcept {
    __try {
        if(address<0x10000)return false;
        std::memcpy(output.position.data(),reinterpret_cast<const void*>(address),12);
        std::memcpy(output.rotation.data(),reinterpret_cast<const void*>(address+16),16);
        return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
EditorSetupReceipt CaptureEditorSetup(uintptr_t address,EditorRegistrationDiagnostic* diagnostic=nullptr) {
    if(diagnostic)diagnostic->address=address;
    if(!EditorCaptureCheck(diagnostic,EditorRegistrationStep::CaptureConfigured,cvr::menu_preview::EditorNativeApplicationsConfigured()))return {};
    cvr::camera::PoseFingerprint actual;
    if(!EditorCaptureCheck(diagnostic,EditorRegistrationStep::CaptureFingerprint,ReadEditorSetupFingerprint(address,actual)))return {};
    auto receipt=s_editorCameraTransport.Capture(address,actual);
    if(!EditorCaptureCheck(diagnostic,EditorRegistrationStep::CaptureLedger,bool(receipt)&&bool(receipt.payload)))return {};
    const auto& app=*receipt.payload;
    if(diagnostic)diagnostic->incoming=EditorApplicationSnapshot(&app);
    const bool registered=cvr::menu_preview::EditorNativeSourceRegistered(app.metadata,app.source);
    if(!EditorCaptureCheck(diagnostic,EditorRegistrationStep::CaptureSource,registered))return {};
    const auto now=cvr::menu_preview::PublicationNowMs();
    if(!EditorCaptureCheck(diagnostic,EditorRegistrationStep::CaptureAge,cvr::menu_preview::EditorNativeSetupCaptureAgeAllowed(app,now)))return {};
    const auto age=now-app.metadata.publishedAtMs;
    if(cvr::menu_preview::IsEditorNativePreparation(app)&&age>500){
        static std::array<std::atomic<uint64_t>,2> counts{};
        const auto count=counts[app.source.eye].fetch_add(1,std::memory_order_relaxed)+1;
        if(count<=4||count==128||count==1024)try{Log("[EditorNativePreparationAge] count=%llu address=%llx ageMs=%llu publishedAtMs=%llu nowMs=%llu epoch=%u setup=%u eye=%u key=%llx binding=%llu preparationFrame=%u source=%llu registered=%u; original cached baseline admission only, current Lease/tracking/GPU checks unchanged\n",count,address,age,app.metadata.publishedAtMs,now,app.metadata.screenEpoch,app.metadata.setupID,app.source.eye,app.source.cameraKey,app.source.bindingGeneration,app.frame.nativeFrame,app.frame.sourceSequence,unsigned(registered));}catch(...){}
    }
    return receipt;
}
void TransferEditorSetup(const EditorSetupReceipt& before,uintptr_t destination){
    cvr::camera::PoseFingerprint sourceNow,destinationNow;
    if(!before||!ReadEditorSetupFingerprint(before.source,sourceNow)||!ReadEditorSetupFingerprint(destination,destinationNow)){
        s_editorCameraTransport.Invalidate(destination);return;
    }
    s_editorCameraTransport.Transfer(before,sourceNow,destination,destinationNow);
}
// Keep shared ownership out of the assembly hook's fixed trivial-POD scratch.
// Its address identifies one before/after activation, including nested copies.
struct PendingEditorSetupCopy {void* scratch{};EditorSetupReceipt receipt;};
thread_local std::array<PendingEditorSetupCopy,8> t_editorSetupCopies;
thread_local cvr::menu_preview::EditorCameraCopyTicketStore<8> t_editorCameraCopyTickets;
void EditorSetupCopyBefore(void* scratch,uintptr_t source){
    const auto receipt=CaptureEditorSetup(source);if(!receipt)return;
    for(auto& pending:t_editorSetupCopies)if(!pending.scratch){pending={scratch,receipt};return;}
    // A deeper nesting has no proof; the after callback will invalidate its
    // destination rather than borrow another activation's receipt.
}
void EditorSetupCopyAfter(void* scratch,uintptr_t destination){
    for(auto& pending:t_editorSetupCopies)if(pending.scratch==scratch){
        auto receipt=std::move(pending.receipt);pending={};TransferEditorSetup(receipt,destination);return;
    }
    s_editorCameraTransport.Invalidate(destination);
}
}

uint64_t cvr::menu_preview::BeginEditorNativeCameraCopy(uintptr_t source)noexcept {
    try{return t_editorCameraCopyTickets.Begin(CaptureEditorSetup(source));}catch(...){return 0;}
}
bool cvr::menu_preview::CompleteEditorNativeCameraCopy(uint64_t ticket,uintptr_t destination,uintptr_t caller)noexcept {
    try{
        EditorSetupReceipt before;
        const bool transferred=t_editorCameraCopyTickets.Complete(ticket,s_editorCameraTransport,destination,
            [](uintptr_t address,cvr::camera::PoseFingerprint& out){return ReadEditorSetupFingerprint(address,out);},&before);
        const auto exe=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const bool registered=exe&&caller==exe+0x36FD7C;
        if(registered&&!transferred)try{
            EditorRegistrationDiagnostic diagnostic;diagnostic.address=destination;diagnostic.context=destination>=0x10000+0x1E20?destination-0x1E20:0;
            EditorRegistrationCheck(&diagnostic,EditorRegistrationStep::Transfer,false);
            if(before.payload){diagnostic.incoming=EditorApplicationSnapshot(before.payload.get());NoteEditorRegistration("copy",before.payload->metadata,before.payload->source,diagnostic);}
            else if(diagnostic.context){uint64_t key{};cvr::menu_preview::Lease lease;
                if(ReadPreviewBytes(diagnostic.context+0x28,&key,sizeof(key))&&cvr::menu_preview::TryAcquireForVirtualCamera(key,lease)&&lease.source){
                    diagnostic.key=key;diagnostic.leasePublished=lease.metadata.publishedAtMs;NoteEditorRegistration("copy",lease.metadata,*lease.source,diagnostic);}
            }
        }catch(...){}
        const bool retained=registered&&transferred&&RetainEditorRegisteredPreparation(before,destination);
        if(registered&&!retained)s_editorCameraTransport.Invalidate(destination);
        if(ticket&&before&&before.payload&&before.payload->source.eye<2){
            static std::array<std::atomic<unsigned>,2> rows{};
            const auto& application=*before.payload;const auto eye=application.source.eye;
            if(rows[eye].fetch_add(1,std::memory_order_relaxed)<12){
                Log("[EditorNativeDual] camera-copy ticket=%llu transferred=%u sourceAddress=%llx destination=%llx callerRva=%llx frame=%u sourceSequence=%llu key=%llx eye=%u registered=%u retained=%u; exact before/after native copy ancestry, no tracked application or GPU claim\n",
                    ticket,unsigned(transferred),before.source,destination,exe&&caller>=exe?caller-exe:0,
                    application.frame.nativeFrame,application.frame.sourceSequence,application.source.cameraKey,eye,unsigned(registered),unsigned(retained));
            }
        }
        return registered?retained:transferred;
    }catch(...){try{s_editorCameraTransport.Invalidate(destination);}catch(...){}return false;}
}

// These engine leaf functions have a narrower clobber set than the Win64 ABI.
// In particular, +682209 calls SetupDefault with a live object in R9 and uses
// R9 again immediately after the call. C++ bookkeeping must be transparent.
extern "C" {
void* CvrPoseSetupCopyOriginal{};
void* CvrPoseSetupDefaultOriginal{};
uintptr_t CvrPoseSetupCopyDetour(uintptr_t,uintptr_t);
uintptr_t CvrPoseSetupDefaultDetour(uintptr_t);
void CvrPoseSetupDefaultBefore(uintptr_t destination) {
    cvr::camera::InvalidatePoseAddress(destination);
    s_editorCameraTransport.Invalidate(destination);
}
void CvrPoseSetupCopyBefore(void* storage,uintptr_t source) {
    using cvr::camera::PoseReceipt;
    static_assert(sizeof(PoseReceipt)<=0x200 && alignof(PoseReceipt)<=16);
    static_assert(std::is_trivially_destructible_v<PoseReceipt>);
    new (storage) PoseReceipt(cvr::camera::CapturePoseAddress(source));
    EditorSetupCopyBefore(storage,source);
}
void CvrPoseSetupCopyAfter(cvr::camera::PoseReceipt* receipt,uintptr_t destination) {
    using cvr::camera::PoseReceipt;
    cvr::camera::TransferPoseAddress(*receipt,destination);
    EditorSetupCopyAfter(receipt,destination);
    receipt->~PoseReceipt();
}
}

namespace {
using namespace cvr::camera;
using CopyFn=uintptr_t(__fastcall*)(uintptr_t,uintptr_t);
using SerializeFn=void(__fastcall*)(uintptr_t,uintptr_t);
using RttFn=double(__fastcall*)(uintptr_t,uintptr_t,char);
using RenderFn=uintptr_t(__fastcall*)(uintptr_t,uintptr_t,uintptr_t);
using BlendFn=uintptr_t(__fastcall*)(uintptr_t);
using BlendFinalizeFn=void(__fastcall*)(uintptr_t);
using RttRefreshFn=void(__fastcall*)(uintptr_t);
CopyFn s_descriptor{},s_cameraClone{};
SerializeFn s_serialize{};
SerializeFn s_genericSerialize{};
RttFn s_rtt{};
RenderFn s_render{},s_mainRead{};
BlendFn s_blend{};
BlendFinalizeFn s_blendFinalize{};
RttRefreshFn s_rttRefresh{};
uintptr_t s_exe{};
static cvr::menu_preview::CameraTestMode PreviewCameraMode(){
    static const auto mode=[] {char value[24]{},local[24]{};const auto n=GetEnvironmentVariableA("CPVR_MENU_STEREO_CAMERA_TEST",value,sizeof(value));
        const auto l=GetEnvironmentVariableA("CPVR_MENU_LOCAL_CAMERA_TEST",local,sizeof(local));
        if((l==4 && std::strcmp(local,"left")==0)||(l==5 && std::strcmp(local,"right")==0))return cvr::menu_preview::CameraTestMode::Off;
        return n && n<sizeof(value)?cvr::menu_preview::ParseCameraTestMode(value):cvr::menu_preview::CameraTestMode::Off;}();return mode;
}
static bool LocalCameraObservationEnabled(){
    static const bool enabled=[] {char value[24]{};const auto n=GetEnvironmentVariableA("CPVR_MENU_LOCAL_CAMERA_TEST",value,sizeof(value));
        return (cvr::menu_preview::UiStereoRequested()||(n==4 && std::strcmp(value,"left")==0)||(n==5 && std::strcmp(value,"right")==0)) && PreviewCameraMode()==cvr::menu_preview::CameraTestMode::Off;}();return enabled;
}
std::mutex s_appliedProducerMutex;
std::array<cvr::menu_preview::AppliedProducer,16> s_appliedProducers{};
size_t s_appliedProducerNext{};
thread_local cvr::menu_preview::Lease t_previewProducer{};
struct PreviewProducerSample {
    cvr::menu_preview::Metadata metadata{};
    cvr::menu_preview::Descriptor descriptor{};
    uintptr_t destination{};
};
std::mutex s_previewMutex;
std::array<PreviewProducerSample,32> s_previewSamples{};
size_t s_previewNext{};
std::atomic<bool> s_previewSamplesReady{};
std::atomic<uint32_t> s_previewProbeRows{};
bool ReadPreviewBytes(uintptr_t address,void* output,size_t size) {
    __try {if(address<0x10000)return false;std::memcpy(output,reinterpret_cast<const void*>(address),size);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void TracePreviewDescriptor(uintptr_t destination,uintptr_t descriptor) {
    if(!t_previewProducer.lifetime)return;
    PreviewProducerSample sample;sample.metadata=t_previewProducer.metadata;sample.destination=destination;
    if(!ReadPreviewBytes(descriptor,sample.descriptor.data(),sample.descriptor.size()))return;
    {std::lock_guard lock(s_previewMutex);s_previewSamples[s_previewNext++%s_previewSamples.size()]=sample;}
    s_previewSamplesReady.store(true,std::memory_order_release);
    if(s_previewProbeRows.fetch_add(1,std::memory_order_relaxed)>=96)return;
    float optics[4]{};std::memcpy(optics,sample.descriptor.data()+0x38,sizeof(optics));
    float rotation[4]{};std::memcpy(rotation,sample.descriptor.data(),sizeof(rotation));
    int32_t position[3]{};std::memcpy(position,sample.descriptor.data()+0x10,sizeof(position));
    Log("[MenuPreview] producer-descriptor component=%llx epoch=%u setup=%u scene=%llx virtual=%llx source=%llx destination=%llx posFP=%d,%d,%d quat=%g,%g,%g,%g optics38=%g,%g,%g,%g; observation only\n",
        sample.metadata.componentIdentity,sample.metadata.screenEpoch,sample.metadata.setupID,sample.metadata.sceneNameHash,sample.metadata.virtualCameraNameHash,
        descriptor,destination,position[0],position[1],position[2],double(rotation[0]),double(rotation[1]),double(rotation[2]),double(rotation[3]),
        double(optics[0]),double(optics[1]),double(optics[2]),double(optics[3]));
}
void TracePreviewRender(uintptr_t destination,uintptr_t descriptor,uintptr_t context) {
    if(!s_previewSamplesReady.load(std::memory_order_acquire))return;
    // Numeric match is a candidate join, not proven registration ancestry.
    // Reacquire exact typed identity before using an older producer sample.
    uint8_t bytes[28]{};if(!ReadPreviewBytes(descriptor,bytes,sizeof(bytes)))return;
    PreviewProducerSample match{};unsigned count=0;
    {std::lock_guard lock(s_previewMutex);
        for(const auto& sample:s_previewSamples){
            if(!sample.destination || std::memcmp(sample.descriptor.data(),bytes,28))continue;
            if(count && match.metadata.componentIdentity==sample.metadata.componentIdentity &&
               match.metadata.screenEpoch==sample.metadata.screenEpoch && match.metadata.setupID==sample.metadata.setupID)continue;
            match=sample;++count;
        }}
    if(count!=1)return;
    cvr::menu_preview::Lease lease;
    if(!cvr::menu_preview::TryAcquireForComponent(match.metadata.componentIdentity,lease) ||
       lease.metadata.screenEpoch!=match.metadata.screenEpoch || lease.metadata.setupID!=match.metadata.setupID)return;
    if(s_previewProbeRows.fetch_add(1,std::memory_order_relaxed)>=96)return;
    uint64_t key{};ReadPreviewBytes(context+0x28,&key,sizeof(key));
    uint8_t setup[32]{};
    const bool setupPoseMatches=ReadPreviewBytes(context+0x1E20,setup,sizeof(setup)) &&
        !std::memcmp(setup,bytes+16,12) && !std::memcmp(setup+16,bytes,16);
    Log("[MenuPreview] render-setup-candidate component=%llx epoch=%u setup=%u virtual=%llx nativeContext=%llx key=%llx registeredSetup=%llx producerDestination=%llx source=%llx destination=%llx setupPoseMatch=%u; pose match only, registration unproven\n",
        lease.metadata.componentIdentity,lease.metadata.screenEpoch,lease.metadata.setupID,lease.metadata.virtualCameraNameHash,
        context,key,context+0x1E20,match.destination,descriptor,destination,unsigned(setupPoseMatches));
}
thread_local PoseReceipt t_source{};
thread_local uintptr_t t_blendDirector{};
thread_local uintptr_t t_rttComponent{};
thread_local uintptr_t t_serializingComponent{};
struct ExternalHead {OpenXRHeadPose head{};uint64_t id{};};
thread_local ExternalHead t_externalHead{};
std::mutex s_blendDebugMutex;
std::mutex s_externalDebugMutex[2];

bool ExternalAllowed() {
    // The existing device-lens and BD playback paths already own those views.
    return g_menuModeValue==0 && OpenXRManager::Get().IsSessionRunning() && !DeviceCamActive() && !(g_bdActive.load(std::memory_order_relaxed) &&
                                  g_bdScenePoseValid.load(std::memory_order_acquire));
}
bool ExternalHeadRead(ExternalHead& sample,bool includesFpp) {
    if(includesFpp) {
        const auto component=g_camObjMain.load(std::memory_order_acquire);
        const auto source=component ? CapturePoseAddress(component+0xE0):PoseReceipt{};
        if(!source || source.payload.view!=1 || !source.payload.head.valid ||
           source.payload.head.originSerial!=OpenXRManager::Get().GetTrackingOriginSerial())return false;
        sample={source.payload.head,source.payload.poseId};return true;
    }
    // A dormant FPP component may carry an old, still valid label. Use it only
    // when the director is actually blending that component into this view.
    uint64_t sequence{};cvr::roomscale::Vec2 consumed{};
    if(!OpenXRManager::Get().AcquireCameraPoseFrame(&sample.head,&consumed,&sequence) || !sample.head.valid)return false;
    sample.id=sequence ? sequence:NextPoseIdentity();return true;
}
bool SelectionIncludesFpp(uintptr_t director) {
    // Called inside the native serializer loop, which already holds its lock.
    uintptr_t table{};uint8_t count{};
    if(!ReadU64Safe(director+0x48,&table)||!ReadU8Safe(director+0x54,&count)||!table||count>64)return false;
    const auto main=g_camObjMain.load(std::memory_order_acquire);
    for(uint32_t i=0;i<count;++i) {
        uintptr_t camera{};float weight{};
        if(ReadU64Safe(table+i*0x20,&camera)&&camera==main&&ReadFloatSafe(table+i*0x20+0x10,&weight)&&weight>0)return true;
    }
    return false;
}
bool ReadExternalPose(uintptr_t address,ExternalViewPose& pose) {
    for(int i=0;i<3;++i){uint32_t value{};if(!ReadU32Safe(address+i*4,&value))return false;pose.position[i]=int32_t(value);}
    return ReadFloatArraySafe(reinterpret_cast<const float*>(address+16),&pose.rotation.x,4) && NormalizeExternal(pose.rotation);
}
bool WriteExternalPose(uintptr_t address,const ExternalViewPose& pose) {
    for(int i=0;i<3;++i)if(!WriteU32Safe(address+i*4,uint32_t(pose.position[i])))return false;
    for(int i=0;i<4;++i)if(!WriteFloatSafe(address+16+i*4,(&pose.rotation.x)[i]))return false;
    return true;
}
void RecordExternal(uint32_t eye,uintptr_t address,uintptr_t component,const ExternalHead& sample) {
    ExternalViewPose pose{};float fov{};
    if(!ReadExternalPose(address,pose)||!ReadFloatSafe(address+0x20,&fov))return;
    std::lock_guard lock(s_externalDebugMutex[eye]);
    auto& out=CyberpunkVR_ExternalCameraDebug[eye];
    CyberpunkVR_ExternalCameraDebugSeq[eye].fetch_add(1,std::memory_order_acq_rel);
    ++out.writes;out.stampUs=XrDiagNowUs();out.poseId=sample.id;out.origin=sample.head.originSerial;out.component=component;
    std::memcpy(out.position,pose.position.data(),12);std::memcpy(out.rotation,&pose.rotation,16);
    const auto& h=sample.head;
    const float head[]{h.posX,h.posY,h.posZ,h.oriX,h.oriY,h.oriZ,h.oriW};
    std::memcpy(out.head,head,sizeof(head));out.fov=fov;
    CyberpunkVR_ExternalCameraDebugSeq[eye].fetch_add(1,std::memory_order_release);
}
bool ComposeExternalMain(const ExternalViewPose& base,const ExternalHead& head,ExternalViewPose& output) {
    const auto& h=head.head;
    const XrPosef pose{{h.oriX,h.oriY,h.oriZ,h.oriW},{h.posX,h.posY,h.posZ}};
    const float eye=CyberpunkVR_IpdInWorldPos ? GetDesiredHalfIpd()*(CyberpunkVR_MainIsRightEye ? 1.f:-1.f):0;
    return ComposeExternal(base,pose,GetWorldScale(),eye,output);
}
struct ExternalSelection {ExternalViewPose main{};ExternalHead sample{};float fov{};bool externalView{};};
thread_local const ExternalSelection* t_externalRtt{};
std::mutex s_externalMainMutex;
ExternalViewLease<ExternalSelection> s_externalMain;
ExternalViewLease<ExternalSelection> s_aimMain;
cvr::WeakObjectSlot<RED4ext::WeakHandle<RED4ext::ISerializable>> s_externalVrcam;
void ObserveExternalVrcam(uintptr_t component) {
    // Only called with the live argument of the engine's RTT callbacks.
    if(!component)return;
    const auto* object=reinterpret_cast<RED4ext::ent::IComponent*>(component);
    if(object->ref.instance==object && object->ref.refCount)
        s_externalVrcam.Bind(component,reinterpret_cast<uintptr_t>(object->ref.refCount),object->ref);
}
ExternalViewKey ExternalKey() {
    return {g_camObjMain.load(std::memory_order_acquire),
        cvr::detail::g_vrcam_comp.load(std::memory_order_acquire),OpenXRManager::Get().GetTrackingOriginSerial()};
}
bool SceneMainHandoff() {
    // This existing scene/editor route already positions VRCAM from MAIN.
    // Carry the completed MAIN receipt with that transform as well, instead
    // of losing it at the legacy component rewrite (and disabling framegen).
    return CyberpunkVR_BdEditorAlign && g_bdActive.load(std::memory_order_relaxed) &&
        !g_bdScenePoseValid.load(std::memory_order_acquire);
}
void NotifyExternalVrcam(uintptr_t component) {
    static thread_local bool pushing=false;
    uintptr_t table{},notify{};
    if(pushing || !component || !ReadU64Safe(component,&table) || !table || !ReadU64Safe(table+0x240,&notify) || !notify)return;
    pushing=true;
    reinterpret_cast<void(__fastcall*)(uintptr_t,uintptr_t)>(notify)(component,component+0x100);
    pushing=false;
}
void PublishExternalMain(uintptr_t address,const PoseReceipt& receipt) {
    if(!receipt)return; // missing observation does not mean an FPP transition
    const auto key=ExternalKey();
    if(!ExternalAllowed()) {
        std::lock_guard lock(s_externalMainMutex);s_externalMain.Clear();s_aimMain.Clear();return;
    }
    ExternalSelection frame{};
    if(receipt.payload.head.originSerial!=key.origin || !ReadExternalPose(address,frame.main) ||
       !ReadFloatSafe(address+0x20,&frame.fov))return;
    frame.sample={receipt.payload.head,receipt.payload.poseId};
    frame.externalView=receipt.payload.externalView;
    const bool mirror=frame.externalView || SceneMainHandoff();
    {
        std::lock_guard lock(s_externalMainMutex);
        const auto now=XrDiagNowUs();
        s_aimMain.Publish(frame,key,now);
        if(mirror)s_externalMain.Publish(frame,key,now);else s_externalMain.Clear();
    }
    if(!mirror)return;
    // Refresh from this completed MAIN, without one-frame lag. The address is
    // only an identity comparison: locking the engine weak reference is what
    // proves lifetime, including if the old component has already been freed.
    const auto live=s_externalVrcam.Lock(key.eyeCamera);
    if(!live){CVR_DIAGNOSTIC(++CyberpunkVR_ExternalNotify[1]);return;}
    const auto* component=static_cast<RED4ext::ent::IComponent*>(live.GetPtr());
    if(!component->isEnabled || !component->owner ||
       key.eyeCamera!=cvr::detail::g_vrcam_comp.load(std::memory_order_acquire)) {
        CVR_DIAGNOSTIC(++CyberpunkVR_ExternalNotify[2]);return;
    }
    NotifyExternalVrcam(key.eyeCamera); // live holds the component through the call
    CVR_DIAGNOSTIC(++CyberpunkVR_ExternalNotify[0]);
}
bool ReadExternalMain(ExternalSelection& output) {
    const auto key=ExternalKey();
    std::lock_guard lock(s_externalMainMutex);
    if(!ExternalAllowed() || OpenXRManager::Get().ExternalPoseResetPending()) {
        s_externalMain.Clear();CVR_DIAGNOSTIC(++CyberpunkVR_ExternalSelectionStates[2]);return false;
    }
    const auto now=XrDiagNowUs();
    if(s_externalMain.Read(key,now,output)) {
        if(!output.externalView && !SceneMainHandoff()) {s_externalMain.Clear();return false;}
        CVR_DIAGNOSTIC(++CyberpunkVR_ExternalSelectionStates[now-s_externalMain.stamp>50000?1:0]);return true;
    }
    CVR_DIAGNOSTIC(++CyberpunkVR_ExternalSelectionStates[3]);return false;
}

uintptr_t __fastcall Blend(uintptr_t director) {
    const auto previous=t_blendDirector;
    const auto previousHead=t_externalHead;t_externalHead={};
    t_blendDirector=director;
    const auto result=s_blend(director);
    t_blendDirector=previous;
    t_externalHead=previousHead;
    return result;
}
void __fastcall BlendFinalize(uintptr_t array) {
    // The native blend is finished, but its serialized inputs still exist.
    // This call site is under our Blend scope; unrelated array frees never
    // acquire a camera label. Read the actual entries, not a table sampled
    // before the director took its own lock.
    if(t_blendDirector && reinterpret_cast<uintptr_t>(_ReturnAddress())==s_exe+0x127A62) {
        uintptr_t entries{};uint32_t count{};
        const bool diagnostics=cvr::RuntimeDiagnosticsEnabled();
        PoseBlendDebug diagnostic{};
        diagnostic.director=t_blendDirector;
        const auto output=t_blendDirector+0x4C0;
        bool accepted=false;
        if(ReadU64Safe(array,&entries) && ReadU32Safe(array+12,&count) && count>0 && count<=64) {
            std::vector<WeightedPoseReceipt> inputs;
            inputs.reserve(count);
            bool readable=true;
            for(uint32_t i=0;i<count;++i) {
                const auto entry=entries+uintptr_t(i)*0xA0;
                float weight{};
                if(!ReadFloatSafe(entry+0x90,&weight)) { readable=false;break; }
                const auto receipt=CapturePoseAddress(entry);
                inputs.push_back({receipt,weight});
                if(diagnostics && i<8) {
                    diagnostic.weights[i]=weight;
                    diagnostic.poseIds[i]=receipt.payload.poseId;
                    diagnostic.components[i]=receipt.payload.component;
                    if(receipt)diagnostic.inputs[i]=receipt.fingerprint;
                }
            }
            if(readable)accepted=PublishBlendedPose(output,inputs);
            else InvalidatePoseAddress(output);
        } else InvalidatePoseAddress(output);
        if(accepted)PublishExternalMain(output,CapturePoseAddress(output));
        if(MainEyeSourceRequested()){
            const auto receipt=CapturePoseAddress(output);
            RecordMainEyeTransportJoin(MainEyeTransportJoin::Blend,output,receipt && bool(receipt.payload.mainEyeSource));
        }
        if(diagnostics) {
        diagnostic.count=count;
        diagnostic.accepted=accepted ? 1u : 0u;
        if(const auto receipt=CapturePoseAddress(output))diagnostic.output=receipt.fingerprint;
        std::lock_guard lock(s_blendDebugMutex);
        diagnostic.calls=CyberpunkVR_PoseBlendDebug.calls+1;
        diagnostic.labelled=CyberpunkVR_PoseBlendDebug.labelled+(accepted ? 1 : 0);
        diagnostic.rejected=CyberpunkVR_PoseBlendDebug.rejected+(accepted ? 0 : 1);
        CyberpunkVR_PoseBlendDebugSeq.fetch_add(1,std::memory_order_acq_rel);
        CyberpunkVR_PoseBlendDebug=diagnostic;
        CyberpunkVR_PoseBlendDebugSeq.fetch_add(1,std::memory_order_release);
        }
    }
    s_blendFinalize(array);
}

struct SourceScope {
    PoseReceipt previous;
    explicit SourceScope(const PoseReceipt& value):previous(t_source) { t_source=value; }
    ~SourceScope() { t_source=previous; }
};
bool DescriptorMatches(uintptr_t descriptor,const PoseReceipt* receipt) {
    if(!receipt || !*receipt || descriptor<0x10000)return false;
    __try {
        return std::memcmp(reinterpret_cast<const void*>(descriptor),receipt->fingerprint.rotation.data(),16)==0 &&
               std::memcmp(reinterpret_cast<const void*>(descriptor+16),receipt->fingerprint.position.data(),12)==0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void __fastcall Serialize(uintptr_t interfacePointer,uintptr_t destination) {
    const auto source=CapturePoseAddress(interfacePointer-0x120+0xE0);
    InvalidatePoseAddress(destination);
    const auto previous=t_serializingComponent;
    t_serializingComponent=interfacePointer-0x120;
    s_serialize(interfacePointer,destination); // includes the existing LocateCamera hook
    t_serializingComponent=previous;
    // Locate owns takeover transforms and publishes their actual written pose.
    // The destination was invalidated before this call, so only a new, still
    // matching publication can survive here. Ordinary serialization inherits
    // the component receipt as before.
    const bool sceneSourceWriter=MainEyeSourceRequested() && g_bdActive.load(std::memory_order_relaxed) &&
        g_bdScenePoseValid.load(std::memory_order_acquire);
    if(!CapturePoseAddress(destination) && !sceneSourceWriter)PublishSerializedPose(destination,source);
    if(MainEyeSourceRequested()){
        const auto receipt=CapturePoseAddress(destination);
        RecordMainEyeTransportJoin(MainEyeTransportJoin::Serialize,destination,receipt && bool(receipt.payload.mainEyeSource));
    }
}
void __fastcall GenericSerialize(uintptr_t interfacePointer,uintptr_t destination) {
    InvalidatePoseAddress(destination);
    s_genericSerialize(interfacePointer,destination);
    if(!t_blendDirector || !ExternalAllowed())return;
    RefreshPlayerCameraState();
    if(!t_externalHead.id && !ExternalHeadRead(t_externalHead,SelectionIncludesFpp(t_blendDirector)))return;
    ExternalViewPose base{},composed{};
    if(!ReadExternalPose(destination,base)||!ComposeExternalMain(base,t_externalHead,composed)||
       !WriteExternalPose(destination,composed))return;
    const float fov=g_normalFovOverrideValue;
    if(std::isfinite(fov)&&fov>1&&fov<179)WriteFloatSafe(destination+0x20,fov);
    PublishCameraSetupPose(destination,interfacePointer-0x120,1,t_externalHead.id,t_externalHead.head,true);
    CVR_DIAGNOSTIC(RecordExternal(0,destination,interfacePointer-0x120,t_externalHead));
}
uintptr_t __fastcall MainRead(uintptr_t director,uintptr_t destination,uintptr_t setup) {
    // Bind the actual selected setup argument. Reading director+4C0 here would
    // mislabel an override chosen by the director after an earlier observation.
    auto selected=CapturePoseAddress(setup);
    const bool sourceRequested=MainEyeSourceRequested();
    if(sourceRequested && !AdmitMainEyeSelectedCamera(selected,setup))selected={};
    SourceScope scope(selected);
    if(!t_source)CVR_DIAGNOSTIC(CyberpunkVR_PoseMainReadMissing[setup==director+0x4C0 ? 0 : 1].fetch_add(1,std::memory_order_relaxed));
    InvalidatePoseAddress(destination);
    const auto result=s_mainRead(director,destination,setup);
    const bool transferred=TransferPoseAddress(t_source,destination);
    if(sourceRequested)RecordMainEyeTransportJoin(MainEyeTransportJoin::MainRead,destination,transferred && bool(t_source.payload.mainEyeSource));
    PublishExternalMain(destination,CapturePoseAddress(destination));
    return result;
}
double __fastcall RttBuild(uintptr_t component,uintptr_t destination,char force) {
    cvr::menu_preview::Lease preview;
    cvr::menu_preview::TryAcquireForComponent(component,preview);
    cvr::menu_preview::ScopedProducer previewScope(t_previewProducer,std::move(preview));
    if(t_previewProducer.lifetime && s_previewProbeRows.fetch_add(1,std::memory_order_relaxed)<96)
        Log("[MenuPreview] producer-begin component=%llx epoch=%u setup=%u virtual=%llx destination=%llx force=%u; observation only\n",
            component,t_previewProducer.metadata.screenEpoch,t_previewProducer.metadata.setupID,
            t_previewProducer.metadata.virtualCameraNameHash,destination,unsigned(uint8_t(force)));
    // AC2BA4 reads this component's +E0/+F0 before publishing its named view.
    // Capture BEFORE the read, including when the resulting coordinates repeat.
    SourceScope scope(CapturePoseAddress(component+0xE0));
    if(component && component==cvr::detail::g_vrcam_comp.load(std::memory_order_acquire))ObserveExternalVrcam(component);
    ExternalSelection selection{};
    const bool external=component==cvr::detail::g_vrcam_comp.load(std::memory_order_acquire) && ReadExternalMain(selection);
    const auto previousSelection=t_externalRtt;t_externalRtt=external?&selection:nullptr;
    const auto previous=t_rttComponent;t_rttComponent=component;
    const bool nativeEditor=t_previewProducer.source&&t_previewProducer.source->mode==cvr::menu_preview::NativePreviewMode::NativeEditorDual;
    const auto result=s_rtt(component,destination,(external||nativeEditor)?1:force);
    t_rttComponent=previous;t_externalRtt=previousSelection;return result;
}
void __fastcall RttRefresh(uintptr_t component) {
    cvr::menu_preview::Lease preview;
    const bool trace=cvr::menu_preview::TryAcquireForComponent(component,preview);
    static std::mutex probeMutex;
    static uint32_t emitted{},lastEpoch{},lastSetup{};
    static uint64_t lastTick{};
    bool emit=false;
    if(trace) {
        const uint64_t now=GetTickCount64();
        std::lock_guard probeLock(probeMutex);
        const bool changed=preview.metadata.screenEpoch!=lastEpoch || preview.metadata.setupID!=lastSetup;
        if(emitted<32 && (!emitted || changed || now-lastTick>=500)) {
            ++emitted;lastTick=now;lastEpoch=preview.metadata.screenEpoch;lastSetup=preview.metadata.setupID;emit=true;
        }
    }
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    uint32_t dimensions[2]{};uint64_t name{};uintptr_t resource{};
    const bool readable=emit && ReadPreviewBytes(component+0x258,dimensions,sizeof(dimensions)) &&
        ReadPreviewBytes(component+0x260,&name,sizeof(name)) && ReadPreviewBytes(component+0x1E8,&resource,sizeof(resource));
    if(emit)Log("[MenuPreview] rtt-refresh-begin component=%llx epoch=%u setup=%u virtual=%llx name=%llx dims=%ux%u resource=%llx readable=%u caller=%llx callerRva=%llx; observation only\n",
        component,preview.metadata.screenEpoch,preview.metadata.setupID,preview.metadata.virtualCameraNameHash,
        name,dimensions[0],dimensions[1],resource,unsigned(readable),caller,caller>=s_exe?caller-s_exe:0);
    if(component && component==cvr::detail::g_vrcam_comp.load(std::memory_order_acquire)) {
        ObserveExternalVrcam(component);
        ExternalSelection selection{};
        if(ReadExternalMain(selection))NotifyExternalVrcam(component);
    }
    s_rttRefresh(component);
    if(emit)Log("[MenuPreview] rtt-refresh-return component=%llx epoch=%u setup=%u; native return is void, not GPU completion\n",
        component,preview.metadata.screenEpoch,preview.metadata.setupID);
}
uintptr_t __fastcall DescriptorBuild(uintptr_t destination,uintptr_t descriptor) {
    // Every native write revokes the previous editor ancestry, including a
    // different camera reusing this address with numerically identical bytes.
    s_editorCameraTransport.Invalidate(destination);
    if(t_previewProducer.lifetime&&t_previewProducer.source&&
       t_previewProducer.source->mode==cvr::menu_preview::NativePreviewMode::NativeEditorDual&&
       t_previewProducer.source->component==t_rttComponent&&
       reinterpret_cast<uintptr_t>(_ReturnAddress())==s_exe+0xAC2CE5){
        cvr::menu_preview::Descriptor native{};uint32_t preparationFrame{};
        if(ReadPreviewBytes(descriptor,native.data(),native.size())&&
           cvr::framegen::ReadRenderFrameIndex(&preparationFrame)){
            // This is only original native-camera ancestry. The engine caches
            // this packet before registered views are assembled on a later
            // renderer clock. Keep head/IPD/mapped pose OUT of that cache.
            cvr::menu_preview::CameraApplication application;
            application.metadata=t_previewProducer.metadata;application.source=*t_previewProducer.source;
            const auto& m=application.metadata;
            application.frame={{m.screenEpoch,m.setupID,m.componentIdentity,m.virtualCameraNameHash},
                preparationFrame,application.source.eye,NextPoseIdentity(),true};
            application.baseDescriptor=native;application.eyeDescriptor=native;
            // projection.valid=false/nativeContext=0 explicitly mark this as
            // private preparation proof, never a public tracked application.
            InvalidatePoseAddress(destination);s_editorCameraTransport.Invalidate(destination);
            const auto result=s_descriptor(destination,descriptor);
            cvr::camera::PoseFingerprint actual;
            const bool accepted=ReadEditorSetupFingerprint(destination,actual)&&
                s_editorCameraTransport.Publish(destination,actual,std::make_shared<const cvr::menu_preview::CameraApplication>(application));
            static std::atomic<unsigned> rows{};
            if(rows.fetch_add(1,std::memory_order_relaxed)<12)Log("[EditorNativeDual] prepared-native frame=%u eye=%u source=%llu generation=%llu component=%llx key=%llx destination=%llx accepted=%u; original unmapped cache only, not a tracked application\n",
                application.frame.nativeFrame,application.frame.eye,application.frame.sourceSequence,application.source.bindingGeneration,
                application.source.component,application.source.cameraKey,destination,unsigned(accepted));
            return result;
        }
        InvalidatePoseAddress(destination);
        s_editorCameraTransport.Invalidate(destination);
        cvr::menu_preview::ClearCameraApplicationForSource(*t_previewProducer.source);
        return s_descriptor(destination,descriptor);
    }
    const auto mode=PreviewCameraMode();
    if(mode!=cvr::menu_preview::CameraTestMode::Off && t_previewProducer.lifetime &&
       t_previewProducer.metadata.componentIdentity==t_rttComponent &&
       reinterpret_cast<uintptr_t>(_ReturnAddress())==s_exe+0xAC2CE5){
        cvr::menu_preview::AppliedProducer applied;applied.metadata=t_previewProducer.metadata;
        uint32_t frame{};const bool clock=cvr::framegen::ReadRenderFrameIndex(&frame);
        const float half=GetDesiredHalfIpd();
        if(clock && half>0 && ReadPreviewBytes(descriptor,applied.base.data(),applied.base.size())){
            static cvr::menu_preview::PreviewFrameEyeOwner owner;
            {std::lock_guard lock(s_appliedProducerMutex);applied.source=owner.Resolve(applied.metadata,applied.metadata.virtualCameraNameHash,clock,frame);}
            if(applied.source){
                if(mode==cvr::menu_preview::CameraTestMode::Left)applied.source.eye=0;
                if(mode==cvr::menu_preview::CameraTestMode::Right)applied.source.eye=1;
                const float displacement=mode==cvr::menu_preview::CameraTestMode::Zero?0.f:(applied.source.eye?half:-half);
                if(cvr::menu_preview::MakeEyeDescriptor(applied.base,displacement,applied.applied)){
                    InvalidatePoseAddress(destination);
                    const auto result=s_descriptor(destination,reinterpret_cast<uintptr_t>(applied.applied.data()));
                    {std::lock_guard lock(s_appliedProducerMutex);s_appliedProducers[s_appliedProducerNext++%s_appliedProducers.size()]=applied;}
                    static std::atomic<uint32_t> rows{};
                    if(rows.fetch_add(1,std::memory_order_relaxed)<32)Log("[MenuPreview] early-camera-applied epoch=%u setup=%u component=%llx key=%llx producerFrame=%u producerSource=%llu eye=%u destination=%llx mode=%s; registered producer only\n",
                        applied.metadata.screenEpoch,applied.metadata.setupID,applied.metadata.componentIdentity,applied.metadata.virtualCameraNameHash,
                        frame,applied.source.sourceSequence,mode==cvr::menu_preview::CameraTestMode::Zero?UINT32_MAX:applied.source.eye,destination,
                        mode==cvr::menu_preview::CameraTestMode::Zero?"zero-copy-diagnostic":"eye");
                    return result;
                }
            }
        }
    }
    if(t_previewProducer.lifetime && t_previewProducer.metadata.componentIdentity==t_rttComponent &&
       reinterpret_cast<uintptr_t>(_ReturnAddress())==s_exe+0xAC2CE5)
        TracePreviewDescriptor(destination,descriptor);
    if(t_rttComponent && t_rttComponent==cvr::detail::g_vrcam_comp.load(std::memory_order_acquire) &&
       reinterpret_cast<uintptr_t>(_ReturnAddress())==s_exe+0xAC2CE5) {
        if(t_externalRtt) {
            const auto& source=*t_externalRtt;
            auto opposite=source.main;
            const float offset=CyberpunkVR_IpdInWorldPos ? GetDesiredHalfIpd()*2*(CyberpunkVR_MainIsRightEye?-1.f:1.f):0;
            if(ExternalEye(opposite,offset)) {
                // This is the native builder's temporary descriptor, not the
                // player's placed component. Matrices/culling are constructed
                // by the original from these fields; no late camera rewrite.
                alignas(16) std::array<std::byte,0x60> copy{};
                std::memcpy(copy.data(),reinterpret_cast<const void*>(descriptor),copy.size());
                std::memcpy(copy.data(),&opposite.rotation,16);
                std::memcpy(copy.data()+0x10,opposite.position.data(),12);
                const float fov=source.fov;
                if(std::isfinite(fov)&&fov>1&&fov<179)std::memcpy(copy.data()+0x38,&fov,4);
                InvalidatePoseAddress(destination);
                const auto result=s_descriptor(destination,reinterpret_cast<uintptr_t>(copy.data()));
                PublishCameraSetupPose(destination,t_rttComponent,2,source.sample.id,source.sample.head,source.externalView);
                CVR_DIAGNOSTIC(RecordExternal(1,destination,t_rttComponent,source.sample));
                return result;
            }
        }
    }
    const auto source=DescriptorMatches(descriptor,&t_source) ? t_source : PoseReceipt{};
    InvalidatePoseAddress(destination);
    const auto result=s_descriptor(destination,descriptor);
    const bool transferred=TransferPoseAddress(source,destination);
    if(MainEyeSourceRequested())RecordMainEyeTransportJoin(MainEyeTransportJoin::Descriptor,destination,transferred && bool(source.payload.mainEyeSource));
    return result;
}
bool RetainEditorRegisteredPreparation(const EditorSetupReceipt& before,uintptr_t destination) {
    using namespace cvr::menu_preview;
    EditorRegistrationDiagnostic diagnostic;diagnostic.address=destination;diagnostic.incoming=EditorApplicationSnapshot(before.payload.get());
    const auto failed=[&]{if(before.payload)NoteEditorRegistration("registration",before.payload->metadata,before.payload->source,diagnostic);return false;};
    if(!EditorRegistrationCheck(&diagnostic,EditorRegistrationStep::Before,bool(before)&&bool(before.payload))||
       !EditorRegistrationCheck(&diagnostic,EditorRegistrationStep::Preparation,IsEditorNativePreparation(*before.payload))||
       !EditorRegistrationCheck(&diagnostic,EditorRegistrationStep::Destination,destination>=0x10000+0x1E20))return failed();
    const auto& prepared=*before.payload;const uintptr_t context=destination-0x1E20;
    diagnostic.context=context;
    uint64_t key{};Lease lease;
    const bool keyKnown=ReadPreviewBytes(context+0x28,&key,sizeof(key));diagnostic.key=key;
    if(!EditorRegistrationCheck(&diagnostic,EditorRegistrationStep::Key,keyKnown)||
       !EditorRegistrationCheck(&diagnostic,EditorRegistrationStep::Lease,TryAcquireForVirtualCamera(key,lease)))return failed();
    diagnostic.leasePublished=lease.metadata.publishedAtMs;
    if(!EditorRegistrationCheck(&diagnostic,EditorRegistrationStep::LeaseSource,bool(lease.source))||
       !EditorRegistrationCheck(&diagnostic,EditorRegistrationStep::Source,prepared.source==*lease.source)||
       !EditorRegistrationCheck(&diagnostic,EditorRegistrationStep::Scope,SameEditorApplicationScope(prepared.metadata,lease.metadata))||
       !EditorRegistrationCheck(&diagnostic,EditorRegistrationStep::Registered,EditorNativeSourceRegistered(lease.metadata,*lease.source)))return failed();
    std::array<uint8_t,0x48> bytes{};Descriptor native{};
    if(!EditorRegistrationCheck(&diagnostic,EditorRegistrationStep::NativeRead,ReadPreviewBytes(destination,bytes.data(),bytes.size())))return failed();
    NoteEditorNativeFields(diagnostic,bytes);
    if(!EditorRegistrationCheck(&diagnostic,EditorRegistrationStep::Reconstruct,ReadRegisteredEditorDescriptor(bytes,prepared.baseDescriptor,native)))return failed();
    // A named camera can be registered only on native dirty updates. Keep its
    // original native bytes and exact preparation ancestry for every render.
    diagnostic.failure=EditorRegistrationStep::Accepted;
    NoteEditorRegistration("baseline",lease.metadata,*lease.source,diagnostic);
    static std::atomic<unsigned> rows{};
    if(rows.fetch_add(1,std::memory_order_relaxed)<16)Log("[EditorNativeDual] baseline-registered preparationFrame=%u eye=%u key=%llx context=%llx; original native camera and preparation retained, tracked application belongs to each RenderBuild\n",
        prepared.frame.nativeFrame,prepared.source.eye,key,context);
    return true;
}
uintptr_t __fastcall CameraClone(uintptr_t destination,uintptr_t source) {
    const auto receipt=CapturePoseAddress(source);
    const auto editorReceipt=CaptureEditorSetup(source);
    const auto result=s_cameraClone(destination,source);
    TransferPoseAddress(receipt,destination);TransferEditorSetup(editorReceipt,destination);return result;
}
uintptr_t __fastcall RenderBuild(uintptr_t destination,uintptr_t descriptor,uintptr_t clipPlanes) {
    // The verified call at4E50F9 passes context+20A0. Its descriptor is made
    // from context+1E20, populated by the registered-camera copy (36FD7C).
    PoseReceipt source{};
    if(reinterpret_cast<uintptr_t>(_ReturnAddress())==s_exe+0x4E50FE && clipPlanes>0x20A0) {
        const auto context=clipPlanes-0x20A0;
        if(PreviewCameraMode()!=cvr::menu_preview::CameraTestMode::Off || LocalCameraObservationEnabled()){
            uint64_t key{};cvr::menu_preview::Lease preview;
            cvr::menu_preview::Descriptor native{};
            if(ReadPreviewBytes(context+0x28,&key,sizeof(key)) && cvr::menu_preview::TryAcquireForVirtualCamera(key,preview) &&
               ReadPreviewBytes(descriptor,native.data(),native.size())){
                if(preview.metadata.screen==2){
                    if(preview.source&&preview.source->mode==cvr::menu_preview::NativePreviewMode::NativeEditorDual){
                        EditorRegistrationDiagnostic diagnostic;diagnostic.context=context;diagnostic.key=key;diagnostic.leasePublished=preview.metadata.publishedAtMs;
                        EditorRegistrationCheck(&diagnostic,EditorRegistrationStep::Key,true);
                        EditorRegistrationCheck(&diagnostic,EditorRegistrationStep::Lease,true);
                        EditorRegistrationCheck(&diagnostic,EditorRegistrationStep::LeaseSource,true);
                        EditorRegistrationCheck(&diagnostic,EditorRegistrationStep::NativeRead,true);
                        const auto early=CaptureEditorSetup(context+0x1E20,&diagnostic);
                        const auto& original=early.payload;
                        const bool baseline=EditorRegistrationCheck(&diagnostic,EditorRegistrationStep::LateReceipt,bool(early))&&
                            EditorRegistrationCheck(&diagnostic,EditorRegistrationStep::LatePayload,bool(original))&&
                            EditorRegistrationCheck(&diagnostic,EditorRegistrationStep::Preparation,cvr::menu_preview::IsEditorNativePreparation(*original))&&
                            EditorRegistrationCheck(&diagnostic,EditorRegistrationStep::LateSource,original->source==*preview.source)&&
                            EditorRegistrationCheck(&diagnostic,EditorRegistrationStep::LateScope,cvr::menu_preview::SameEditorApplicationScope(original->metadata,preview.metadata))&&
                            EditorRegistrationCheck(&diagnostic,EditorRegistrationStep::LateDescriptor,cvr::menu_preview::EditorNativeRenderPreparationMatches(*original,preview.metadata,*preview.source,native));
                        if(!baseline&&diagnostic.failure==EditorRegistrationStep::LateDescriptor&&original){
                            diagnostic.descriptorKnown=true;diagnostic.poseMatch=cvr::menu_preview::EditorSetupFingerprint(original->eyeDescriptor)==cvr::menu_preview::EditorSetupFingerprint(native);
                            std::memcpy(diagnostic.expectedOptics,original->eyeDescriptor.data()+0x38,12);std::memcpy(diagnostic.actualOptics,native.data()+0x38,12);
                            diagnostic.opticsMatch=std::memcmp(diagnostic.expectedOptics,diagnostic.actualOptics,12)==0;
                        }
                        cvr::menu_preview::CameraApplication applied;bool matched=false;
                        if(baseline){
                            const auto compose=[&](const cvr::menu_preview::Descriptor& input,cvr::menu_preview::CameraApplication& output){return cvr::menu_preview::ComposeSpatialEditorNativeFrame(preview,input,output,&diagnostic.compose);};
                            matched=EditorRegistrationCheck(&diagnostic,EditorRegistrationStep::Compose,cvr::menu_preview::BuildEditorNativeRenderApplication(*original,preview.metadata,*preview.source,native,compose,applied));
                            if(matched){diagnostic.incoming=EditorApplicationSnapshot(&applied);
                                matched=EditorRegistrationCheck(&diagnostic,EditorRegistrationStep::LateProjection,applied.projection.valid)&&
                                    EditorRegistrationCheck(&diagnostic,EditorRegistrationStep::LateClock,cvr::framegen::ReadRenderFrameIndex(&diagnostic.frame))&&
                                    EditorRegistrationCheck(&diagnostic,EditorRegistrationStep::LateFrame,applied.frame.nativeFrame==diagnostic.frame)&&
                                    EditorRegistrationCheck(&diagnostic,EditorRegistrationStep::LateOrigin,cvr::menu_preview::EditorNativeRenderFrameCurrent(applied,diagnostic.frame,(diagnostic.origin=OpenXRManager::Get().GetTrackingOriginSerial())))&&
                                    EditorRegistrationCheck(&diagnostic,EditorRegistrationStep::Registered,cvr::menu_preview::EditorNativeSourceRegistered(preview.metadata,*preview.source));
                            }
                        }
                        InvalidatePoseAddress(destination);
                        const auto renderDescriptor=matched?reinterpret_cast<uintptr_t>(applied.eyeDescriptor.data()):descriptor;
                        const auto result=s_render(destination,renderDescriptor,clipPlanes); // Per-render tracked descriptor; registered native camera stays original.
                        bool published=false;
                        if(matched){applied.nativeContext=context;applied.nativeDestination=destination;applied.sourceDescriptor=renderDescriptor;
                            diagnostic.incoming=EditorApplicationSnapshot(&applied);
                            const bool current=EditorRegistrationCheck(&diagnostic,EditorRegistrationStep::AfterClock,cvr::framegen::ReadRenderFrameIndex(&diagnostic.afterFrame))&&
                                EditorRegistrationCheck(&diagnostic,EditorRegistrationStep::AfterFrame,applied.frame.nativeFrame==diagnostic.afterFrame)&&
                                EditorRegistrationCheck(&diagnostic,EditorRegistrationStep::AfterOrigin,cvr::menu_preview::EditorNativeRenderFrameCurrent(applied,diagnostic.afterFrame,(diagnostic.afterOrigin=OpenXRManager::Get().GetTrackingOriginSerial())))&&
                                EditorRegistrationCheck(&diagnostic,EditorRegistrationStep::AfterSource,cvr::menu_preview::EditorNativeSourceRegistered(preview.metadata,*preview.source));
                            if(current)published=EditorRegistrationCheck(&diagnostic,EditorRegistrationStep::LatePublish,cvr::menu_preview::PublishCameraApplicationIfCurrent(std::make_shared<cvr::menu_preview::CameraApplication>(std::move(applied))));}
                        if(!published){NoteEditorRegistration("late",preview.metadata,*preview.source,diagnostic,true);cvr::menu_preview::ClearCameraApplicationForSource(*preview.source);}
                        else{diagnostic.failure=EditorRegistrationStep::Accepted;NoteEditorRegistration("late",preview.metadata,*preview.source,diagnostic);}
                        static std::atomic<unsigned> rows{};
                        if(rows.fetch_add(1,std::memory_order_relaxed)<16)Log("[EditorNativeDual] registered-view frame=%u eye=%u key=%llx context=%llx ancestry=%u matched=%u published=%u; actual native camera consumed, GPU pending\n",
                            diagnostic.frame,preview.source->eye,key,context,unsigned(bool(early)),unsigned(matched),unsigned(published));
                        return result;
                    }
                    cvr::menu_preview::CameraApplication applied;
                    if(cvr::menu_preview::ComposeSpatialEditorFrame(preview.metadata,native,applied)){
                        InvalidatePoseAddress(destination);
                        const auto result=s_render(destination,reinterpret_cast<uintptr_t>(applied.eyeDescriptor.data()),clipPlanes);
                        applied.nativeContext=context;applied.nativeDestination=destination;applied.sourceDescriptor=descriptor;
                        cvr::menu_preview::PublishCameraApplicationIfCurrent(std::make_shared<cvr::menu_preview::CameraApplication>(std::move(applied)));
                        return result;
                    }
                    cvr::menu_preview::ClearCameraApplication();
                    return s_render(destination,descriptor,clipPlanes);
                }
                static std::atomic<uint32_t> originRows{};
                bool emitOrigin=false;
                if(LocalCameraObservationEnabled()){
                    static std::mutex originMutex;static uint32_t emitted{};static uint64_t lastTick{};static std::array<uint8_t,12> lastPosition{};
                    const auto now=GetTickCount64();
                    std::lock_guard lock(originMutex);
                    if(emitted<8 && (!emitted || std::memcmp(lastPosition.data(),native.data()+0x10,12) || now-lastTick>=500)){
                        ++emitted;lastTick=now;std::memcpy(lastPosition.data(),native.data()+0x10,12);emitOrigin=true;
                    }
                }else emitOrigin=originRows.fetch_add(1,std::memory_order_relaxed)<8;
                if(emitOrigin){
                    int32_t position[3]{};float auxiliary[4]{},origin[4]{};
                    std::memcpy(position,native.data()+0x10,12);std::memcpy(auxiliary,native.data()+0x20,16);
                    const bool originRead=ReadPreviewBytes(clipPlanes,origin,sizeof(origin));
                    Log("[MenuPreview] render-origin-observed epoch=%u setup=%u context=%llx FP=%d,%d,%d descriptor20=%g,%g,%g,%g R8=%g,%g,%g,%g readable=%u expectedFPFloat=%g,%g,%g; vectors unchanged\n",
                        preview.metadata.screenEpoch,preview.metadata.setupID,context,position[0],position[1],position[2],
                        double(auxiliary[0]),double(auxiliary[1]),double(auxiliary[2]),double(auxiliary[3]),
                        double(origin[0]),double(origin[1]),double(origin[2]),double(origin[3]),unsigned(originRead),
                        double(position[0])/131072.,double(position[1])/131072.,double(position[2])/131072.);
                }
                if(LocalCameraObservationEnabled()){
                    const auto local=cvr::menu_preview::ReadLocalComponentPose();uint32_t frame{};float origin[4]{};
                    if(local && cvr::menu_preview::MatchesLocalComponentPose(*local,preview.metadata,native) &&
                       ReadPreviewBytes(clipPlanes,origin,sizeof(origin)) && cvr::menu_preview::MatchesLocalRenderOrigin(*local,origin) &&
                       cvr::framegen::ReadRenderFrameIndex(&frame)){
                        static std::mutex localConsumerMutex;static cvr::menu_preview::ConsumingFrameOwner localConsumer;
                        cvr::menu_preview::PreviewFrameEye selection;
                        {std::lock_guard lock(localConsumerMutex);selection=localConsumer.Resolve(preview.metadata,true,frame,local->eye);}
                        if(selection){
                            InvalidatePoseAddress(destination);
                            const auto result=s_render(destination,descriptor,clipPlanes); // supported component already updated; NO offset
                            auto receipt=std::make_shared<cvr::menu_preview::CameraApplication>();
                            receipt->metadata=preview.metadata;receipt->frame=selection;
                            receipt->localPoseRevision=local->revision;
                            receipt->nativeContext=context;receipt->nativeDestination=destination;receipt->sourceDescriptor=descriptor;
                            receipt->baseDescriptor=native;receipt->eyeDescriptor=native;
                            std::memcpy(receipt->baseDescriptor.data(),local->baseline.data()+16,16);
                            std::memcpy(receipt->baseDescriptor.data()+16,local->baseline.data(),12);
                            const bool accepted=cvr::menu_preview::PublishLocalCameraApplicationIfCurrent(std::move(receipt));
                            static std::atomic<uint32_t> rows{};
                            if(selection.changed && rows.fetch_add(1,std::memory_order_relaxed)<32)
                                Log("[MenuPreview] local-component-camera-consumed epoch=%u setup=%u component=%llx key=%llx context=%llx nativeFrame=%u source=%llu eye=%u accepted=%u; exact component pose match, GPU/UI ownership pending\n",
                                    preview.metadata.screenEpoch,preview.metadata.setupID,preview.metadata.componentIdentity,key,context,frame,selection.sourceSequence,selection.eye,unsigned(accepted));
                            return result;
                        }
                    }
                }
                cvr::menu_preview::AppliedProducer matched{};bool found=false,ambiguous=false;
                {std::lock_guard lock(s_appliedProducerMutex);
                    for(const auto& p:s_appliedProducers)if(cvr::menu_preview::MatchesAppliedProducer(p,preview.metadata,native)){
                        if(found && matched.source.eye!=p.source.eye){ambiguous=true;break;}
                        if(!found || p.source.sourceSequence>matched.source.sourceSequence)matched=p;
                        found=true;
                    }}
                uint32_t frame{};const bool clock=cvr::framegen::ReadRenderFrameIndex(&frame);
                if(PreviewCameraMode()==cvr::menu_preview::CameraTestMode::Zero){
                    static std::atomic<uint32_t> rows{};
                    if(rows.fetch_add(1,std::memory_order_relaxed)<8)
                        Log("[MenuPreview] zero-copy-diagnostic render context=%llx nativeFrame=%u clock=%u producerMatched=%u; physical eye publication rejected\n",
                            context,frame,unsigned(clock),unsigned(found&&!ambiguous));
                    // No physical eye receipt: this A/B tests copy/rebuild path
                    // at zero displacement, not a geometrically distinct eye.
                    return s_render(destination,descriptor,clipPlanes);
                }
                if(found && !ambiguous && clock){
                    static std::mutex consumerMutex;static cvr::menu_preview::ConsumingFrameOwner consumer;
                    cvr::menu_preview::PreviewFrameEye selection;
                    {std::lock_guard lock(consumerMutex);selection=consumer.Resolve(preview.metadata,clock,frame,matched.source.eye);}
                    if(selection){
                        InvalidatePoseAddress(destination);
                        const auto result=s_render(destination,descriptor,clipPlanes); // NO second offset
                        auto receipt=std::make_shared<cvr::menu_preview::CameraApplication>();
                        receipt->metadata=preview.metadata;receipt->frame=selection;
                        receipt->nativeContext=context;receipt->nativeDestination=destination;receipt->sourceDescriptor=descriptor;
                        receipt->baseDescriptor=matched.base;receipt->eyeDescriptor=matched.applied;
                        cvr::menu_preview::PublishCameraApplicationIfCurrent(std::move(receipt));
                        static std::atomic<uint32_t> rows{};
                        if(selection.changed && rows.fetch_add(1,std::memory_order_relaxed)<32)
                            Log("[MenuPreview] camera-applied epoch=%u setup=%u component=%llx key=%llx context=%llx nativeFrame=%u source=%llu eye=%u producerFrame=%u matchedProducer=%llu; early pose consumed, UI/GPU ownership unproven\n",
                                preview.metadata.screenEpoch,preview.metadata.setupID,preview.metadata.componentIdentity,key,context,
                                frame,selection.sourceSequence,selection.eye,matched.source.nativeFrame,matched.source.sourceSequence);
                        return result;
                    }
                }
            }
        }
        TracePreviewRender(destination,descriptor,context);
        source=CapturePoseAddress(context+0x1E20);
        if(!DescriptorMatches(descriptor,&source))source={};
    }
    InvalidatePoseAddress(destination);
    const auto result=s_render(destination,descriptor,clipPlanes);
    TransferPoseAddress(source,destination);return result;
}

bool Matches(const cvr::camera::sites::Site& site) {
    const auto* bytes=reinterpret_cast<const uint8_t*>(s_exe+site.rva);
    for(size_t i=0;site.bytes[i*2];++i) {
        unsigned expected{};
        if(std::sscanf(site.bytes+i*2,"%2x",&expected)!=1 || bytes[i]!=expected)return false;
    }
    return true;
}
struct Site { cvr::camera::sites::Site expected;void* hook;void** original; };
bool InstallCameraPoseTransport() {
    s_exe=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if(!s_exe)return false;
    const Site sites[]={
        {cvr::camera::sites::Blend,reinterpret_cast<void*>(&Blend),reinterpret_cast<void**>(&s_blend)},
        {cvr::camera::sites::BlendFinalize,reinterpret_cast<void*>(&BlendFinalize),reinterpret_cast<void**>(&s_blendFinalize)},
        {cvr::camera::sites::Serialize,reinterpret_cast<void*>(&Serialize),reinterpret_cast<void**>(&s_serialize)},
        {cvr::camera::sites::GenericSerialize,reinterpret_cast<void*>(&GenericSerialize),reinterpret_cast<void**>(&s_genericSerialize)},
        {cvr::camera::sites::SetupCopy,reinterpret_cast<void*>(&CvrPoseSetupCopyDetour),&CvrPoseSetupCopyOriginal},
        {cvr::camera::sites::SetupDefault,reinterpret_cast<void*>(&CvrPoseSetupDefaultDetour),&CvrPoseSetupDefaultOriginal},
        {cvr::camera::sites::MainRead,reinterpret_cast<void*>(&MainRead),reinterpret_cast<void**>(&s_mainRead)},
        {cvr::camera::sites::RttBuild,reinterpret_cast<void*>(&RttBuild),reinterpret_cast<void**>(&s_rtt)},
        {cvr::camera::sites::RttRefresh,reinterpret_cast<void*>(&RttRefresh),reinterpret_cast<void**>(&s_rttRefresh)},
        {cvr::camera::sites::DescriptorBuild,reinterpret_cast<void*>(&DescriptorBuild),reinterpret_cast<void**>(&s_descriptor)},
        {cvr::camera::sites::CameraClone,reinterpret_cast<void*>(&CameraClone),reinterpret_cast<void**>(&s_cameraClone)},
        {cvr::camera::sites::RenderBuild,reinterpret_cast<void*>(&RenderBuild),reinterpret_cast<void**>(&s_render)}
    };
    for(const auto& site:sites)if(!Matches(site.expected))return false;
    if(!Matches(cvr::camera::sites::RenderCall) || !Matches(cvr::camera::sites::BlendFinalizeCall) ||
       !Matches(cvr::camera::sites::RttDescriptorCall)||
       !EnsureCameraPoseCopyHook())return false;
    bool ok=true;
    for(const auto& site:sites) {
        void* target=reinterpret_cast<void*>(s_exe+site.expected.rva);
        const auto created=MH_CreateHook(target,site.hook,site.original);
        const auto enabled=created==MH_OK ? MH_EnableHook(target) : created;
        if(created!=MH_OK || enabled!=MH_OK) {
            Log("PoseIdentity: hook +%llX failed create=%d enable=%d\n",site.expected.rva,int(created),int(enabled));
            ok=false;
        }
    }
    return ok;
}
CVR_HOOK("CameraPoseTransport",::cvr::hooks::Stage::Boot,81,InstallCameraPoseTransport);
}

uintptr_t cvr::camera::CurrentSerializedCameraComponent() {return t_serializingComponent;}

bool cvr::camera::ReadMainAimPose(MainAimPose& out) {
    if(!ExternalAllowed() || OpenXRManager::Get().ExternalPoseResetPending())return false;
    ExternalSelection frame{};const auto key=ExternalKey();const auto now=XrDiagNowUs();
    {
        std::lock_guard lock(s_externalMainMutex);
        if(!s_aimMain.Read(key,now,frame) || now-s_aimMain.stamp>100000)return false;
    }
    const float offset=CyberpunkVR_IpdInWorldPos ? GetDesiredHalfIpd()*(CyberpunkVR_MainIsRightEye?-1.f:1.f):0;
    if(!ExternalEye(frame.main,offset))return false;
    MainAimPose value{};
    for(int i=0;i<3;++i)value.position[i]=float(frame.main.position[i])/131072.f;
    std::memcpy(value.rotation,&frame.main.rotation,sizeof(value.rotation));
    value.poseId=frame.sample.id;out=value;return true;
}

bool cvr::camera::ReadExternalMainDirection(float out[3],uint64_t* poseId) {
    if(!out || !ExternalAllowed() || OpenXRManager::Get().ExternalPoseResetPending())return false;
    ExternalSelection frame{};const auto key=ExternalKey();const auto now=XrDiagNowUs();
    {
        std::lock_guard lock(s_externalMainMutex);
        // Steering needs a recent observation even though a missing MAIN update
        // is never a reason to move the rendered second eye back into the cabin.
        if(!s_externalMain.Read(key,now,frame)||!frame.externalView||now-s_externalMain.stamp>100000)return false;
    }
    XrVector3f forward{};if(!PlanarExternalForward(frame.main.rotation,forward))return false;
    out[0]=forward.x;out[1]=forward.y;out[2]=0;
    if(poseId)*poseId=frame.sample.id;return true;
}

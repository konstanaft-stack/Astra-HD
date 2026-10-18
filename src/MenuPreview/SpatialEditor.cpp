#include "MenuPreview/SpatialEditor.hpp"
#include "MenuPreview/InitialEditorFraming.hpp"
#include "Runtimes/EditorUiLayerPolicy.hpp"
#include "MenuPreview/SpatialEditorSource.hpp"
#include "MenuPreview/UiConsumer.hpp"
#include "MenuPreview/UiCameraCycle.hpp"
#include "MenuPreview/EditorUiSurface.hpp"
#include "MenuPreview/EditorNativeSpatialFrame.hpp"
#include "MenuPreview/EditorCameraLifetime.hpp"
#include "Hooks/Hook.hpp"
#include "Camera/CameraState.hpp"
#include "Core/VrCoreShared.hpp"
#include "Framegen/Inputs.hpp"
#include <MinHook.h>
#include <windows.h>

extern "C" {
std::atomic<uint32_t> CvrEditorCopyActive{};
void* CvrEditorCopyOriginal{};
void* CvrEditorCopyDetour(void*,const void*,size_t);
}
namespace cvr::menu_preview {
namespace {
std::mutex editorMutex;
InitialEditorFraming initialFraming;
cvr::xr::EditorOrbitViewLatch orbitViewLatch;
Metadata referenceScope{};
uint64_t referenceOrigin{};
XrPosef referenceHead{};
EditorCaptureOwner frameOwner;
unsigned cameraRows{};
float requestedHeight{},requestedDistance{},frameHeight{},frameDistance{},requestedOrbit{},frameOrbit{};
uint64_t controlOwner{},controlSequence{};
EditorControllerPolicy controllerPolicy;
EditorControlLatch controlLatch;
uint64_t controlRevisionCounter{},frameControlRevision{},nextControllerLog{};
std::atomic<uint64_t> presentedControlRevision{};
bool controllerClaimed{};
struct NativeSpatialCommon:EditorNativeSpatialCommon {
    InitialEditorFraming framing;
    EditorCameraLifetime cameraLifetime;
    EditorControlLatch controls;
    uint64_t nextControlRevision{};
};
EditorNativeSpatialFrameCache<NativeSpatialCommon> nativeFrameState;
EditorControlLatch nativeControls;
EditorCameraLifetime nativeCameraLifetime;
Metadata nativeControlScope{};
unsigned nativeCameraRows{};
bool Read(uintptr_t p,void* data,size_t size)noexcept{
    __try{if(p<0x10000)return false;std::memcpy(data,reinterpret_cast<const void*>(p),size);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
}
void ClearInitialEditorFraming()noexcept{try{std::lock_guard lock(editorMutex);initialFraming.Clear();requestedOrbit=frameOrbit=0;orbitViewLatch.Reset();nativeFrameState.Clear();nativeControls.Reset();nativeCameraLifetime.Clear();
    if(nativeControlScope.screenEpoch){nativeControlScope={};requestedHeight=requestedDistance=frameHeight=frameDistance=0;}
}catch(...){}}
InitialFramingResult PublishInitialEditorFraming(const Metadata& m,uint64_t actor,uint64_t actorID,uint64_t player,const std::array<double,3>& p,const std::array<double,4>& q,const std::array<double,3>& staging)noexcept{
 try{uint32_t frame{};const bool frameKnown=cvr::framegen::ReadRenderFrameIndex(&frame);std::lock_guard lock(editorMutex);
    if(m.screen!=2)return InitialFramingResult::Rejected;
    return initialFraming.Publish(m.screenEpoch,m.controllerIdentity,actor,actorID,player,m.entityIdentity,p,q,staging,frame,frameKnown);
 }catch(...){return InitialFramingResult::Pending;}
}
void ClearSpatialEditor()noexcept{
    CvrEditorCopyActive.store(0,std::memory_order_release);
    try{std::lock_guard lock(editorMutex);referenceScope={};referenceOrigin=0;
        controllerPolicy.Reset();controllerClaimed=false;controlLatch.Reset();controlSequence=frameControlRevision=0;
        nativeFrameState.Clear();nativeControls.Reset();nativeCameraLifetime.Invalidate(IsPreviewEpochOpen(nativeCameraLifetime.Epoch()));nativeCameraRows=0;}catch(...){}
}
void NoteSpatialEditorScope(const Metadata& scope)noexcept{
    const bool active=scope.screen==2&&UiStereoRequested();
    CvrEditorCopyActive.store(active?1u:0u,std::memory_order_release);
    if(active){try{const bool nativeDual=EditorNativeApplicationsConfigured();std::lock_guard lock(editorMutex);
        const bool newNativeLifetime=nativeDual&&!SameEditorCameraLifetime(nativeControlScope,scope);
        if(nativeDual)nativeControlScope=scope;
        if(controlOwner!=scope.entityID||newNativeLifetime){controlOwner=scope.entityID;requestedHeight=requestedDistance=frameHeight=frameDistance=requestedOrbit=frameOrbit=0;controlSequence=frameControlRevision=0;controlLatch.Reset();nativeControls.Reset();controllerPolicy.Reset();controllerClaimed=false;}}catch(...){}}
}
void NoteSpatialEditorControlsPresented(uint64_t revision)noexcept{
    if(!revision)return;auto old=presentedControlRevision.load(std::memory_order_relaxed);
    while(old<revision&&!presentedControlRevision.compare_exchange_weak(old,revision,std::memory_order_release,std::memory_order_relaxed)){}
}
bool HandleSpatialEditorController(const EditorControllerSample& sample)noexcept{
    try{
        Lease scope;const bool editor=TryAcquireCurrent(scope)&&scope.metadata.screen==2&&UiStereoRequested();
        EditorControllerStep step;float height{},distance{},orbit{};bool emit{};
        {std::lock_guard lock(editorMutex);
         if(!editor){controllerPolicy.Reset();controllerClaimed=false;return false;}
         step=controllerPolicy.Update(sample);
         if(step.claimed){
             if(step.reset)requestedHeight=requestedDistance=requestedOrbit=0;
             else {requestedHeight=std::clamp(requestedHeight+step.height,-5.f,5.f);
                   requestedDistance=std::clamp(requestedDistance+step.distance,-5.f,5.f);
                   requestedOrbit=AccumulateEditorOrbit(requestedOrbit,step.orbit);}
         }
         const auto now=GetTickCount64();
         emit=step.claimed!=controllerClaimed||step.reset||
              (step.claimed&&(step.height!=0||step.distance!=0||step.orbit!=0)&&now>=nextControllerLog);
         if(emit)nextControllerLog=now+1000;
         controllerClaimed=step.claimed;height=requestedHeight;distance=requestedDistance;orbit=requestedOrbit;}
        if(emit)Log("[SpatialEditor] controller leftGrip=%g stick=%g/%g claimed=%u reset=%u height=%g distance=%g orbit=%g; left grip+left stick height/zoom, left grip+right X camera orbit, editor only\n",
            double(sample.grip),double(sample.stickX),double(sample.stickY),unsigned(step.claimed),unsigned(step.reset),double(height),double(distance),double(orbit));
        return step.claimed;
    }catch(...){return false;}
}
bool HandleSpatialEditorMouse(uint32_t message,uint64_t wparam)noexcept{
    const bool control=(wparam&MK_CONTROL)!=0,shift=(wparam&MK_SHIFT)!=0;
    const bool reset=control&&(message==WM_MBUTTONDOWN||message==WM_MBUTTONUP);
    if(!reset&&(message!=WM_MOUSEWHEEL||(!control&&!shift)))return false;
    try{
        Lease scope;if(!TryAcquireCurrent(scope)||scope.metadata.screen!=2||!UiStereoRequested())return false;
        if(message==WM_MBUTTONUP)return true;
        float height{},distance{};
        {std::lock_guard lock(editorMutex);
         if(reset){requestedHeight=0;requestedDistance=0;requestedOrbit=0;}
         else {const float steps=float(int16_t((wparam>>16)&0xffff))/float(WHEEL_DELTA);
               if(shift)requestedHeight=std::clamp(requestedHeight+steps*.1f,-5.f,5.f);
               else requestedDistance=std::clamp(requestedDistance+steps*.15f,-5.f,5.f);}
         height=requestedHeight;distance=requestedDistance;}
        Log("[SpatialEditor] controls height=%g distance=%g; Shift-wheel height, Ctrl-wheel zoom, Ctrl-middle reset; editor only\n",double(height),double(distance));
        return true;
    }catch(...){return false;}
}
bool ComposeSpatialEditorFrame(const Metadata& scope,const Descriptor& native,CameraApplication& output)noexcept{
    try{
        if(scope.screen!=2||!UiStereoRequested())return false;
        std::array<uint8_t,32> typed{};
        if(!ReadEditorComponentCamera(scope,typed)||!MatchesEditorBaseline(native,typed))return false;
        float aspect{};std::memcpy(&aspect,native.data()+0x3C,4);
        if(!std::isfinite(aspect)||std::abs(aspect-4.f/3.f)>.02f)return false;
        cvr::camera::ExternalViewPose baseline,mapped;
        std::memcpy(baseline.position.data(),native.data()+16,12);std::memcpy(&baseline.rotation,native.data(),16);
        if(!MapEditorCamera(baseline,mapped))return false;
        {std::lock_guard lock(editorMutex);
         std::array<double,3> p{double(mapped.position[0])/131072.,double(mapped.position[1])/131072.,double(mapped.position[2])/131072.};
         std::array<double,4> q{mapped.rotation.x,mapped.rotation.y,mapped.rotation.z,mapped.rotation.w};
         const bool initialOrientation=!initialFraming.oriented;
         if(!initialFraming.Apply(scope.screenEpoch,scope.controllerIdentity,scope.entityIdentity,p,q))return false;
         static unsigned initialRows{};
         if(initialOrientation&&initialRows<4){++initialRows;Log("[EditorAnchor] applied epoch=%u controller=%llx actor=%llx actorID=%llx owner=%llx centre=%g,%g,%g actorYaw=%g delta=%g camera=%g,%g,%g q=%g,%g,%g,%g; first successful orientation only per latch, max4\n",
             initialFraming.epoch,initialFraming.controller,initialFraming.actor,initialFraming.actorID,initialFraming.owner,
             initialFraming.centre[0],initialFraming.centre[1],initialFraming.centre[2],initialFraming.actorYaw,initialFraming.delta,p[0],p[1],p[2],q[0],q[1],q[2],q[3]);}

         for(unsigned i=0;i<3;++i){const double fp=std::round(p[i]*131072.);if(!std::isfinite(fp)||fp<std::numeric_limits<int32_t>::min()||fp>std::numeric_limits<int32_t>::max())return false;mapped.position[i]=int32_t(fp);}
         mapped.rotation={float(q[0]),float(q[1]),float(q[2]),float(q[3])};}

        auto& xr=OpenXRManager::Get();OpenXRHeadPose head{};uint32_t frame{};
        if(!xr.AcquireFrameHeadSample(&head,nullptr,nullptr,nullptr,false)||!head.valid||
           head.originSerial!=xr.GetTrackingOriginSerial()||!cvr::framegen::ReadRenderFrameIndex(&frame))return false;
        const XrPosef relative{{head.oriX,head.oriY,head.oriZ,head.oriW},{head.posX,head.posY,head.posZ}};
        const auto ack=ReadAcknowledgedUiCapture();
        const bool matchingAck=ack&&SamePreview(ack->scope,scope);
        XrPosef anchor;PreviewFrameEye selected;bool emit{};float heightOffset{},distanceOffset{},orbitOffset{};uint64_t controlsRevision{};
        {std::lock_guard lock(editorMutex);
         if(!SamePreview(referenceScope,scope)||referenceOrigin!=head.originSerial){referenceScope=scope;referenceOrigin=head.originSerial;referenceHead=relative;cameraRows=0;}
         anchor=referenceHead;selected=frameOwner.Resolve(scope,frame,matchingAck,matchingAck?ack->eye:0);emit=cameraRows++<4;
         if(selected&&selected.sourceSequence!=controlSequence){
             if(EditorUiSeparationEnabled()){
                 const auto controls=controlLatch.Resolve(selected.sourceSequence,selected.eye,requestedHeight,requestedDistance,
                     presentedControlRevision.load(std::memory_order_acquire),controlRevisionCounter,requestedOrbit);
                 if(!controls.revision)return false;
                 frameHeight=controls.height;frameDistance=controls.distance;frameOrbit=controls.orbit;frameControlRevision=controls.revision;
             }else{frameHeight=requestedHeight;frameDistance=requestedDistance;frameOrbit=requestedOrbit;frameControlRevision=0;}
             controlSequence=selected.sourceSequence;
         }
         heightOffset=frameHeight;distanceOffset=frameDistance;orbitOffset=frameOrbit;controlsRevision=frameControlRevision;}
        if(!selected)return false;
        XrPosef recenter;xr.GetRecenterBase(&recenter);
        const auto width=GetForcedRenderWidthValue(),height=GetForcedRenderHeightValue();
        const float scale=GetWorldScale();
        cvr::camera::ExternalViewPose controlled;
        if(!OffsetEditorCamera(mapped,heightOffset,distanceOffset,scale,controlled))return false;
        EditorOrbitView orbitView;
        if(orbitOffset!=0){std::lock_guard lock(editorMutex);
            EditorOrbitView prepared;const EditorOrbitView* next=nullptr;
            if(orbitViewLatch.NeedsLeft(selected.sourceSequence,selected.eye)){
                if(!cvr::xr::BuildEditorOrbitView(controlled,initialFraming.centre,anchor,recenter,scale,orbitOffset,head.originSerial,controlsRevision,prepared))return false;
                next=&prepared;
            }
            if(!orbitViewLatch.Resolve(selected.key,selected.sourceSequence,selected.eye,head.originSerial,controlsRevision,orbitOffset,next,orbitView))return false;
            std::array<double,3> p{double(controlled.position[0])/131072.,double(controlled.position[1])/131072.,double(controlled.position[2])/131072.};
            std::array<double,4> q{controlled.rotation.x,controlled.rotation.y,controlled.rotation.z,controlled.rotation.w};
            if(!initialFraming.Orbit(scope.screenEpoch,scope.controllerIdentity,scope.entityIdentity,orbitOffset,p,q))return false;
            for(unsigned i=0;i<3;++i){const double fp=std::round(p[i]*131072.);if(!std::isfinite(fp)||fp<std::numeric_limits<int32_t>::min()||fp>std::numeric_limits<int32_t>::max())return false;controlled.position[i]=int32_t(fp);}
            controlled.rotation={float(q[0]),float(q[1]),float(q[2]),float(q[3])};}

        float horizontal=GetForcedFov();if(!(horizontal>1&&horizontal<170))horizontal=xr.GetRuntimeHorizontalFovDeg();
        const auto imageRoute=EditorUiSeparationEnabled()?EditorImageRoute::FullRawImage:EditorImageRoute::LegacyCroppedQuad;
        EditorEye eye;
        if(!std::isfinite(scale)||scale<=0||!BuildEditorEye(controlled,anchor,relative,recenter,scale,
            GetDesiredHalfIpd()/scale,selected.eye,horizontal,width,height,eye,imageRoute))return false;
        CameraApplication result;result.metadata=scope;result.frame=selected;
        result.baseDescriptor=native;result.eyeDescriptor=native;result.localPoseRevision=selected.sourceSequence;
        result.editorControlRevision=controlsRevision;
        std::memcpy(result.eyeDescriptor.data(),&eye.camera.rotation,16);std::memcpy(result.eyeDescriptor.data()+16,eye.camera.position.data(),12);
        std::memcpy(result.eyeDescriptor.data()+0x38,&eye.cameraVerticalFov,4);std::memcpy(result.eyeDescriptor.data()+0x3C,&eye.cameraAspect,4);
        std::memcpy(result.eyeDescriptor.data()+0x40,&eye.nearPlane,4);
        result.projection={eye.localEye,eye.projection,head.originSerial,true};result.projection.orbit=orbitView;output=std::move(result);
        if(emit)Log("[SpatialEditor] native-view epoch=%u setup=%u frame=%u eye=%u source=%llu extent=%ux%u fov=%g aspect=%g fullRaw=%u near=%g baseFP=%d,%d,%d eyeFP=%d,%d,%d; owned typed camera, recovered editor-room map, component unchanged\n",scope.screenEpoch,scope.setupID,frame,selected.eye,selected.sourceSequence,width,height,double(eye.cameraVerticalFov),double(eye.cameraAspect),unsigned(imageRoute==EditorImageRoute::FullRawImage),double(eye.nearPlane),baseline.position[0],baseline.position[1],baseline.position[2],eye.camera.position[0],eye.camera.position[1],eye.camera.position[2]);
        return true;
    }catch(...){return false;}
}

const char* EditorNativeComposeFailureName(EditorNativeComposeFailure failure)noexcept{
    static constexpr const char* names[]{"none","lease-lifetime","lease-source","screen","ui-stereo","scope-fresh","registered-source","head-acquire","head-valid","head-origin","clock","typed-camera","map","initial-framing","pose-write","scale","half-ipd","controls","offset","orbit-view","orbit-framing","orbit-pose","frame-cache","eye-math","exception"};
    const auto index=size_t(failure);return index<std::size(names)?names[index]:"unknown";
}
bool ComposeSpatialEditorNativeFrame(const Lease& lease,const Descriptor& native,CameraApplication& output,EditorNativeComposeFailure* failure)noexcept{
    try{
        const auto fail=[&](EditorNativeComposeFailure reason){if(failure)*failure=reason;return false;};
        if(failure)*failure=EditorNativeComposeFailure::None;
        const auto& scope=lease.metadata;
        if(!lease.lifetime)return fail(EditorNativeComposeFailure::LeaseLifetime);
        if(!lease.source)return fail(EditorNativeComposeFailure::LeaseSource);
        if(scope.screen!=2)return fail(EditorNativeComposeFailure::Screen);
        if(!UiStereoRequested())return fail(EditorNativeComposeFailure::UiStereo);
        if(!FreshPublication(scope.publishedAtMs,PublicationNowMs()))return fail(EditorNativeComposeFailure::ScopeFresh);
        if(!EditorNativeSourceRegistered(scope,*lease.source))return fail(EditorNativeComposeFailure::RegisteredSource);
        auto& xr=OpenXRManager::Get();OpenXRHeadPose head{};uint32_t frame{};
        if(!xr.AcquireFrameHeadSample(&head,nullptr,nullptr,nullptr,false))return fail(EditorNativeComposeFailure::HeadAcquire);
        if(!head.valid)return fail(EditorNativeComposeFailure::HeadValid);
        if(head.originSerial!=xr.GetTrackingOriginSerial())return fail(EditorNativeComposeFailure::HeadOrigin);
        if(!cvr::framegen::ReadRenderFrameIndex(&frame))return fail(EditorNativeComposeFailure::Clock);
        const XrPosef relative{{head.oriX,head.oriY,head.oriZ,head.oriW},{head.posX,head.posY,head.posZ}};
        std::lock_guard lock(editorMutex);
        const auto prepare=[&](const EditorNativeFrameSample& sample,NativeSpatialCommon& common){
            const auto writePose=[](const std::array<double,3>& position,const std::array<double,4>& rotation,cvr::camera::ExternalViewPose& pose){
                for(unsigned i=0;i<3;++i){const double fp=std::round(position[i]*131072.);
                    if(!std::isfinite(fp)||fp<std::numeric_limits<int32_t>::min()||fp>std::numeric_limits<int32_t>::max())return false;pose.position[i]=int32_t(fp);}
                pose.rotation={float(rotation[0]),float(rotation[1]),float(rotation[2]),float(rotation[3])};return true;
            };
            // Category cameras may zoom/orbit automatically. Seed a private
            // base from the first valid primary only, for this editor lifetime.
            // Stage it in the common transaction: failed first-eye math must
            // never freeze a startup sample for the lifetime.
            common.cameraLifetime=nativeCameraLifetime;
            if(!common.cameraLifetime.Resolve(scope,sample.origin,sample.headPose,[&](auto& mapped,auto& framing){
                std::array<uint8_t,32> typed{};
                if(!ReadEditorComponentCamera(scope,typed))return fail(EditorNativeComposeFailure::TypedCamera);
                cvr::camera::ExternalViewPose baseline;
                std::memcpy(baseline.position.data(),typed.data(),12);std::memcpy(&baseline.rotation,typed.data()+16,16);
                if(!MapEditorCamera(baseline,mapped))return fail(EditorNativeComposeFailure::Map);
                framing=initialFraming;
                std::array<double,3> p{double(mapped.position[0])/131072.,double(mapped.position[1])/131072.,double(mapped.position[2])/131072.};
                std::array<double,4> q{mapped.rotation.x,mapped.rotation.y,mapped.rotation.z,mapped.rotation.w};
                if(!framing.Apply(scope.screenEpoch,scope.controllerIdentity,scope.entityIdentity,p,q))return fail(EditorNativeComposeFailure::InitialFraming);
                if(!writePose(p,q,mapped))return fail(EditorNativeComposeFailure::PoseWrite);
                return true;
            }))return false;
            const auto& mapped=common.cameraLifetime.Camera();common.framing=common.cameraLifetime.Framing();
            common.head=sample.headPose;common.origin=sample.origin;
            common.anchor=common.cameraLifetime.Anchor();
            xr.GetRecenterBase(&common.recenter);
            common.width=GetForcedRenderWidthValue();common.height=GetForcedRenderHeightValue();
            common.scale=GetWorldScale();const float desiredHalfIpd=GetDesiredHalfIpd();
            if(!std::isfinite(common.scale)||common.scale<=0)return fail(EditorNativeComposeFailure::Scale);
            if(!std::isfinite(desiredHalfIpd)||desiredHalfIpd<=0)return fail(EditorNativeComposeFailure::HalfIpd);
            common.halfIpd=desiredHalfIpd/common.scale;
            common.horizontal=GetForcedFov();if(!(common.horizontal>1&&common.horizontal<170))common.horizontal=xr.GetRuntimeHorizontalFovDeg();
            const bool separated=EditorUiSeparationEnabled();
            common.imageRoute=separated?EditorImageRoute::FullRawImage:EditorImageRoute::LegacyCroppedQuad;
            common.controls=nativeControls;common.nextControlRevision=controlRevisionCounter;
            float height=requestedHeight,distance=requestedDistance,orbit=requestedOrbit;
            if(separated){
                // A common frame begins with left semantics even when native
                // scheduling visits the fixed right view first.
                const auto controls=common.controls.Resolve(sample.frame.sourceSequence,0,height,distance,
                    presentedControlRevision.load(std::memory_order_acquire),common.nextControlRevision,orbit);
                if(!controls.revision)return fail(EditorNativeComposeFailure::Controls);
                height=controls.height;distance=controls.distance;orbit=controls.orbit;common.controlRevision=controls.revision;
            }
            if(!OffsetEditorCamera(mapped,height,distance,common.scale,common.camera))return fail(EditorNativeComposeFailure::Offset);
            if(orbit!=0){
                if(!cvr::xr::BuildEditorOrbitView(common.camera,common.framing.centre,common.anchor,common.recenter,
                    common.scale,orbit,sample.origin,common.controlRevision,common.orbit))return fail(EditorNativeComposeFailure::OrbitView);
                std::array<double,3> p{double(common.camera.position[0])/131072.,double(common.camera.position[1])/131072.,double(common.camera.position[2])/131072.};
                std::array<double,4> q{common.camera.rotation.x,common.camera.rotation.y,common.camera.rotation.z,common.camera.rotation.w};
                if(!common.framing.Orbit(scope.screenEpoch,scope.controllerIdentity,scope.entityIdentity,orbit,p,q))return fail(EditorNativeComposeFailure::OrbitFraming);
                if(!writePose(p,q,common.camera))return fail(EditorNativeComposeFailure::OrbitPose);
            }
            return true;
        };
        const auto compose=[&](const NativeSpatialCommon& common,const EditorNativeFrameSample& sample,
            const Metadata& canonical,const NativePreviewSource& source,const Descriptor& descriptor,CameraApplication& application){
            if(!BuildEditorNativeSpatialEye(common,sample,canonical,source,descriptor,application))return fail(EditorNativeComposeFailure::EyeMath);
            return true;
        };
        bool firstCommitted=false;
        if(failure)*failure=EditorNativeComposeFailure::FrameCache;
        if(!nativeFrameState.Resolve(scope,*lease.source,frame,head.originSerial,relative,native,prepare,compose,output,&firstCommitted))return false;
        if(firstCommitted){
            const auto& common=*nativeFrameState.CommonState();initialFraming=common.framing;nativeControls=common.controls;
            controlRevisionCounter=common.nextControlRevision;nativeCameraLifetime=common.cameraLifetime;
            if(nativeCameraRows++<4)Log("[SpatialEditor] native-dual-frame epoch=%u setup=%u frame=%u firstEye=%u source=%llu binding=%llu origin=%llu controls=%llu extent=%ux%u; primary typed camera and whole common snapshot frozen at registered camera application\n",
                scope.screenEpoch,scope.setupID,frame,output.frame.eye,output.frame.sourceSequence,output.source.bindingGeneration,common.origin,common.controlRevision,common.width,common.height);
        }
        if(failure)*failure=EditorNativeComposeFailure::None;
        return true;
    }catch(...){if(failure)*failure=EditorNativeComposeFailure::Exception;return false;}
}
}

extern "C" void CvrEditorCopyBefore(void* source,size_t bytes)noexcept{
    if(bytes!=sizeof(cvr::menu_preview::EditorQuad)||!CvrEditorCopyActive.load(std::memory_order_acquire))return;
    try{
        cvr::menu_preview::Lease scope;uint32_t width{},height{};
        if(!cvr::menu_preview::TryAcquireCurrent(scope)||scope.metadata.screen!=2||
           !cvr::menu_preview::ReadUiCanvasExtent(width,height))return;
        cvr::menu_preview::EditorQuad input{},output{};
        const auto eyeWidth=GetForcedRenderWidthValue(),eyeHeight=GetForcedRenderHeightValue();
        const float aspect=eyeHeight?float(eyeWidth)/float(eyeHeight):float(width)/float(height);
        if(!cvr::menu_preview::Read(uintptr_t(source),input.data(),sizeof(input)))return;
        static std::atomic<unsigned> observed{};if(observed.fetch_add(1)<2)Log("[SpatialEditor] vertex-copy-observed epoch=%u setup=%u bytes=%llu canvas=%ux%u; source before native copy, recognition follows\n",scope.metadata.screenEpoch,scope.metadata.setupID,uint64_t(bytes),width,height);
        if(!cvr::menu_preview::ExpandEditorPreview(std::as_bytes(std::span(input)),width,height,output,aspect))return;
        if(!cvr::menu_preview::WriteEditorSourceIfUnchanged(source,input,output))return;
        if(cvr::menu_preview::UiRasterDiagnosticsEnabled()){
            static std::atomic<unsigned> rasterRows{};
            const auto rasterRow=rasterRows.fetch_add(1);if(rasterRow<6)Log("[SpatialEditor] quad-raster-input epoch=%u setup=%u publication=%llu source=%p canvas=%ux%u projectionAspect=%g inputXY=(%g,%g)-(%g,%g) outputXY=(%g,%g)-(%g,%g) uvHalf=(%04X,%04X)-(%04X,%04X); recognized writable source changed before native copy, raster draw logged separately\n",scope.metadata.screenEpoch,scope.metadata.setupID,scope.metadata.publicationSerial,source,width,height,double(aspect),double(input[0].x),double(input[0].y),double(input[2].x),double(input[2].y),double(output[0].x),double(output[0].y),double(output[2].x),double(output[2].y),unsigned(output[0].u),unsigned(output[0].v),unsigned(output[2].u),unsigned(output[2].v));
            if(rasterRow<6)Log("[SpatialEditor] quad-raster-z epoch=%u setup=%u publication=%llu z6=(%g,%g,%g,%g,%g,%g); recognized CPU source only, not a vertex-buffer binding claim\n",scope.metadata.screenEpoch,scope.metadata.setupID,scope.metadata.publicationSerial,double(input[0].z),double(input[1].z),double(input[2].z),double(input[3].z),double(input[4].z),double(input[5].z));
        }
        static std::atomic<unsigned> rows{};if(rows.fetch_add(1)<12)Log("[SpatialEditor] preview-quad epoch=%u setup=%u canvas=%ux%u projectionAspect=%g; source-before-copy RTT geometry, final coverage requires draw evidence\n",scope.metadata.screenEpoch,scope.metadata.setupID,width,height,double(aspect));
    }catch(...){cvr::menu_preview::ClearSpatialEditor();}
}
namespace {
bool InstallSpatialEditorCopy(){
    const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));if(!base)return false;
    const uint8_t expected[]{0x48,0x8B,0xC1,0x4C,0x8D,0x15};
    auto* target=reinterpret_cast<void*>(base+0x1913F10);
    if(std::memcmp(target,expected,sizeof(expected)))return false;
    const auto created=MH_CreateHook(target,reinterpret_cast<void*>(&CvrEditorCopyDetour),&CvrEditorCopyOriginal);
    return created==MH_OK&&MH_EnableHook(target)==MH_OK;
}
CVR_HOOK("SpatialEditorBuffer",::cvr::hooks::Stage::Boot,82,InstallSpatialEditorCopy);
}

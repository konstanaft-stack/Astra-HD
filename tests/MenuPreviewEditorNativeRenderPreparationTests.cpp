#include "MenuPreview/EditorNativeRenderPreparation.hpp"
#include "MenuPreview/EditorNativeSpatialFrame.hpp"
#include <cstdio>
#include <cstdlib>
using namespace cvr::menu_preview;
void require(bool value,const char* why){if(!value){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}}
XrPosef head(float x=0){return {{0,0,0,1},{x,0,0}};}
int main(){
    Metadata m{};m.screen=2;m.screenEpoch=1;m.setupID=6;m.controllerIdentity=3;m.componentIdentity=4;m.virtualCameraNameHash=5;m.entityIdentity=6;m.entityID=7;m.publishedAtMs=1000;
    const std::array<NativePreviewSource,2> sources{{{4,5,0,NativePreviewMode::NativeEditorDual,11},{8,9,1,NativePreviewMode::NativeEditorDual,11}}};
    require(ConfigureEditorNativeApplications(m,11,sources[0],sources[1]),"real current source bindings are registered");
    EditorCameraTransport transport;std::array<std::shared_ptr<CameraApplication>,2> originals;
    std::array<Descriptor,2> incoming;std::array<cvr::camera::PoseFingerprint,2> fingerprints;
    for(unsigned eye=0;eye<2;++eye){auto original=std::make_shared<CameraApplication>();original->metadata=m;original->source=sources[eye];
        original->frame={{1,6,4,5},10,eye,100+eye,true};const float q[4]{0,0,0,1};const int32_t p[3]{100*131072,200*131072,3*131072};
        std::memcpy(original->baseDescriptor.data(),q,16);std::memcpy(original->baseDescriptor.data()+16,p,12);
        const float optics[4]{15,4.f/3,.1f,1000};std::memcpy(original->baseDescriptor.data()+0x38,optics,16);original->eyeDescriptor=original->baseDescriptor;
        originals[eye]=original;incoming[eye]=original->baseDescriptor;fingerprints[eye]=EditorSetupFingerprint(original->baseDescriptor);
        const float currentFov=37,currentFar=1200;std::memcpy(incoming[eye].data()+0x38,&currentFov,4);std::memcpy(incoming[eye].data()+0x44,&currentFar,4);
        require(transport.Publish(0x1000+eye*0x1000,fingerprints[eye],original),"one original registered copy establishes each native camera");}
    const auto originalBytes=incoming;EditorNativeSpatialFrameCache<EditorNativeSpatialCommon> cache;uint32_t frame=20;unsigned commonBuilds=0;
    const auto render=[&](unsigned eye,float x,CameraApplication& out){const auto receipt=transport.Capture(0x1000+eye*0x1000,fingerprints[eye]);if(!receipt||!EditorNativeSourceRegistered(m,receipt.payload->source)||!EditorNativeSetupCaptureAgeAllowed(*receipt.payload,m.publishedAtMs))return false;
        const auto prepare=[&](const EditorNativeFrameSample& sample,EditorNativeSpatialCommon& c){++commonBuilds;c.camera.position={100*131072,200*131072,3*131072};c.camera.rotation={0,0,0,1};c.anchor=c.recenter=head();c.head=sample.headPose;c.origin=sample.origin;c.scale=1;c.halfIpd=.0315f;c.horizontal=94;c.width=1280;c.height=1400;c.imageRoute=EditorImageRoute::FullRawImage;c.controlRevision=1;return true;};
        const auto compose=[&](const Descriptor& native,CameraApplication& app){return cache.Resolve(m,sources[eye],frame,2,head(x),native,prepare,BuildEditorNativeSpatialEye,app);};
        return BuildEditorNativeRenderApplication(*receipt.payload,m,sources[eye],incoming[eye],compose,out);
    };
    CameraApplication previousLeft;
    for(frame=20;frame<180;++frame){m.publishedAtMs=10000+frame;const float x=float(frame-20)*.001f;CameraApplication right,left,repeat;
        require(render(1,x,right)&&EditorNativeRenderFrameCurrent(right,frame,2),"right render advances beyond first copied native frame without another registered copy");
        require(render(0,x+.2f,left)&&EditorNativeRenderFrameCurrent(left,frame,2),"left render has the same current actual native frame");
        require(left.frame.nativeFrame==right.frame.nativeFrame&&left.frame.sourceSequence!=right.frame.sourceSequence&&left.source==sources[0]&&right.source==sources[1],"actual fixed eyes remain independent with a coherent pair");
        require(render(0,x+.5f,repeat)&&SameCameraApplication(left,repeat),"repeated eye is immutable within its actual frame");
        require(left.baseDescriptor==originalBytes[0]&&right.baseDescriptor==originalBytes[1]&&incoming==originalBytes,"native input bytes stay original across all tracked renders");
        if(frame>20)require(left.frame.sourceSequence>previousLeft.frame.sourceSequence,"each new native frame allocates a fresh source identity");previousLeft=left;
        require(!EditorNativeRenderFrameCurrent(left,frame+1,2)&&!EditorNativeRenderFrameCurrent(left,frame,3),"before/after native frame or origin movement rejects publication");
    }
    require(commonBuilds==160,"whole common frame prepares once even with right-first and repeats");
    for(unsigned eye=0;eye<2;++eye)require(transport.Capture(0x1000+eye*0x1000,fingerprints[eye]).payload==originals[eye]&&IsEditorNativePreparation(*originals[eye]),"registered ledger never becomes tracked or accumulates head transforms");
    auto wrong=sources[0];wrong.bindingGeneration++;
    require(!EditorNativeRenderPreparationMatches(*originals[0],m,wrong,incoming[0])&&!EditorNativeRenderPreparationMatches(*originals[0],m,sources[1],incoming[0]),"stale generation and cross-source baseline reject");
    auto category=m;category.setupID++;require(!EditorNativeRenderPreparationMatches(*originals[0],category,sources[0],incoming[0]),"another category cannot reuse old preparation");
    auto changed=incoming[0];changed[16]^=1;require(!EditorNativeRenderPreparationMatches(*originals[0],m,sources[0],changed),"changed native pose cannot borrow original ancestry");
    auto tracked=*originals[0];tracked.projection.valid=true;tracked.nativeContext=0x9000;
    require(!EditorNativeRenderPreparationMatches(tracked,m,sources[0],incoming[0]),"last tracked camera can never become the next original baseline");
    auto next=sources;next[0].bindingGeneration=next[1].bindingGeneration=12;
    require(ConfigureEditorNativeApplications(m,12,next[0],next[1])&&!render(0,.2f,tracked),"authoritative rebinding rejects retained old source even with identical bytes");
    ClearCameraApplication();require(!render(1,.2f,tracked),"authoritative source clear rejects retained original ancestry");
    std::puts("PASS 160 native frames with one original copy, real coherent frame math, right-first/repeat, immutable original bytes and strict source/frame/origin gates");
}

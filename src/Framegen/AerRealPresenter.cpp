#include "Framegen/AerRealPresenter.hpp"
#include "Framegen/NativeGenerationBridge.hpp"
#include "Render/NativeObjectIdentity.hpp"
#include "Utils/XrMath.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace cvr::framegen {
namespace {
bool Number(float v){return std::isfinite(v)&&std::abs(v)<3.4e38f;}
bool Pose(const XrPosef& p){
    const auto&q=p.orientation;
    return Number(q.x)&&Number(q.y)&&Number(q.z)&&Number(q.w)&&
        std::abs(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w-1)<.002f&&
        Number(p.position.x)&&Number(p.position.y)&&Number(p.position.z);
}
XrPosef Compose(const XrPosef&a,const XrPosef&b){
    const auto v=RotateVector(a.orientation,b.position);
    return {MultiplyQuat(a.orientation,b.orientation),{a.position.x+v.x,a.position.y+v.y,a.position.z+v.z}};
}
bool ParallelEye(const XrPosef&p){
    // The recovered matrix rotates in HEAD axes. Canted camera conjugation
    // has not been proved, so do not silently substitute an eye quaternion.
    const auto&q=p.orientation;return Pose(p)&&std::abs(q.x)+std::abs(q.y)+std::abs(q.z)<=.0001f;
}
bool SameDevice(ID3D12DeviceChild*a,ID3D12DeviceChild*b){
    if(!a||!b)return false;
    Microsoft::WRL::ComPtr<ID3D12Device> da,db;
    if(FAILED(a->GetDevice(IID_PPV_ARGS(&da)))||FAILED(b->GetDevice(IID_PPV_ARGS(&db))))return false;
    // ReShade may return its device wrapper from a native resource's GetDevice,
    // while our native queue returns the underlying device. Preserve the exact
    // same-device guard, but compare the registered native identities.
    return cvr::gpu::SameNativeDevice(da.Get(),db.Get());
}
bool Color(DXGI_FORMAT f,CaptureTransfer t){
    switch(f){
    case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return t==CaptureTransfer::SrgbEncoded||t==CaptureTransfer::Linear;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:return t==CaptureTransfer::SrgbEncoded;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:case DXGI_FORMAT_R32G32B32A32_FLOAT:return t==CaptureTransfer::Linear;
    default:return false;
    }
}
AerRealCode Completion(const Frame&f){
    const auto&c=f.colorCompletion;
    if(!c.HasScheduledCopySource()||c.colorState!=CaptureColorReadState)return AerRealCode::InvalidInput;
    const auto done=c.fence->GetCompletedValue();
    if(done==UINT64_MAX)return AerRealCode::DeviceRemoved;
    return done>=c.fenceValue?AerRealCode::Ready:AerRealCode::CapturePending;
}
bool Projection(const CameraConstants&c,AerRealGeometry&g){
    // Native SL row-major cameraViewToClip; same convention as the already
    // verified source_math::projection_fov. Frame.fov is never a fallback.
    if(c.source==CameraConstantsSource::None||c.version<1||c.version>2||!c.flagPresent.test(4)||c.flags[4]!=0)return false;
    for(unsigned i=0;i<16;++i)if(!c.valuePresent.test(i)||!Number(c.values[i]))return false;
    const auto&p=c.values;
    if(!(p[0]>0&&p[5]>0)||std::abs(std::abs(p[11])-1)>.0001f||std::abs(p[15])>.0001f||p[14]==0||
       std::abs(p[1])+std::abs(p[2])+std::abs(p[3])+std::abs(p[4])+std::abs(p[6])+std::abs(p[7])+
       std::abs(p[12])+std::abs(p[13])>.0001f)return false;
    // Optional lens/pinhole fields may be absent/INVALID_FLOAT per SL. When
    // supplied, reject unsupported optics rather than inventing an identity.
    bool lens=true;for(unsigned i=32;i<48;++i)lens&=c.valuePresent.test(i)&&Number(p[i]);
    if(lens)for(unsigned i=32;i<48;++i)if(std::abs(p[i]-(((i-32)%5)==0?1.f:0.f))>.0001f)return false;
    for(unsigned i=84;i<86;++i)if(c.valuePresent.test(i)&&Number(p[i])&&std::abs(p[i])>.0001f)return false;
    const float ox=p[8]/p[11],oy=p[9]/p[11];
    const float left=(1+ox)/p[0],right=(1-ox)/p[0],up=(1-oy)/p[5],down=(1+oy)/p[5];
    if(!(left>0&&right>0&&up>0&&down>0))return false;
    g.renderedFov={-std::atan(left),std::atan(right),std::atan(up),-std::atan(down)};
    const auto&r=g.crop;for(float v:r)if(!Number(v))return false;
    const float width=float(g.width),height=float(g.height);
    if(r[0]<0||r[1]<0||r[2]<=r[0]||r[3]<=r[1]||r[2]>width||r[3]>height)return false;
    const float l=left-r[0]/width*(left+right),rr=-left+r[2]/width*(left+right);
    const float u=up-r[1]/height*(up+down),d=-up+r[3]/height*(up+down);
    if(!(l>0&&rr>0&&u>0&&d>0)||!Number(l)||!Number(rr)||!Number(u)||!Number(d))return false;
    g.tangents={u,d,l,rr};g.effectiveFov={-std::atan(l),std::atan(rr),std::atan(u),-std::atan(d)};return true;
}
bool ResolveGeometry(const CameraConstants& constants,const cvr::ppd::ReferenceOptics& optics,
        unsigned eye,std::optional<std::array<float,4>> crop,AerRealGeometry& geometry){
    if(eye>1)return false;
    geometry.crop=crop.value_or(std::array<float,4>{0,0,float(geometry.width),float(geometry.height)});
    if(!Projection(constants,geometry))return false;
    if(!optics.valid)return true;
    const auto fit=cvr::ppd::CalibrateReferenceOptics(geometry.width,geometry.height,optics);
    const auto expected=cvr::ppd::ReferenceSourceFov(geometry.width,geometry.height,fit);
    const auto& actual=geometry.renderedFov;
    constexpr auto tolerance=AerRealPresenter::CalibrationAngularTolerance;
    if(!fit.valid||std::abs(expected.angleLeft-actual.angleLeft)>tolerance||
       std::abs(expected.angleRight-actual.angleRight)>tolerance||
       std::abs(expected.angleUp-actual.angleUp)>tolerance||
       std::abs(expected.angleDown-actual.angleDown)>tolerance)return false;
    const auto rect=cvr::ppd::ReferenceEyeCrop(geometry.width,geometry.height,fit,optics.applied[eye]);
    if((crop&&*crop!=rect)||rect[2]<=rect[0]||rect[3]<=rect[1])return false;
    geometry.crop=rect;
    const auto t=cvr::ppd::FovTangents(optics.applied[eye]);
    geometry.tangents={t.up,t.down,t.left,t.right};
    const auto& f=optics.applied[eye];geometry.effectiveFov={f.angleLeft,f.angleRight,f.angleUp,f.angleDown};
    return true;
}
}

bool AerRealPresenter::ResolveCapturedGeometry(const CameraConstants& constants,const cvr::ppd::ReferenceOptics& optics,
        unsigned eye,AerRealGeometry& geometry){return ResolveGeometry(constants,optics,eye,{},geometry);}
bool AerRealPresenter::ResolvePairedDisplayGeometry(const Frame& frame,
        std::array<AerRealGeometry,2>& output,bool& baselineCalibration,PairedGeometryDiagnostic* diagnostic){
    if(diagnostic)*diagnostic={};
    const auto reject=[diagnostic](PairedGeometryReason reason,unsigned eye=UINT32_MAX,unsigned component=UINT32_MAX){
        if(diagnostic){diagnostic->reason=reason;diagnostic->eye=eye;diagnostic->component=component;}return false;
    };
    const auto known=[diagnostic](unsigned eye,uint32_t bits){if(diagnostic){diagnostic->eyes[eye].knownBits|=bits;diagnostic->knownBits|=bits<<(1+eye*8);}};
    // The caller owns the live color ReadLease. A scheduled copy is sufficient:
    // the XR submit queue waits its fence after this CPU-only validation.
    // Split the original OR chain in its exact short-circuit order. Diagnostic
    // values never require re-evaluating a predicate/device query after failure.
    if(!frame.identity.serial)return reject(PairedGeometryReason::SerialMissing);
    if(!frame.identity.origin)return reject(PairedGeometryReason::OriginMissing);
    if(!frame.identity.stereo)return reject(PairedGeometryReason::NotStereo);
    if(!frame.nativeStamp.resourceEpoch)return reject(PairedGeometryReason::ResourceEpochMissing);
    if(!frame.color[0])return reject(PairedGeometryReason::LeftColorMissing);
    if(!frame.color[1])return reject(PairedGeometryReason::RightColorMissing);
    if(frame.color[0]==frame.color[1])return reject(PairedGeometryReason::SameColor);
    if(!frame.lease)return reject(PairedGeometryReason::LeaseMissing);
    if(!frame.lease->owner)return reject(PairedGeometryReason::LeaseOwnerMissing);
    if(!frame.lease->release)return reject(PairedGeometryReason::LeaseReleaseMissing);
    if(!frame.colorCompletion.HasScheduledCopySource())return reject(PairedGeometryReason::CopyNotScheduled);
    if(frame.colorCompletion.colorState!=CaptureColorReadState)return reject(PairedGeometryReason::ColorState);
    if(!SameDevice(frame.color[0],frame.color[1]))return reject(PairedGeometryReason::ColorDevice);
    if(diagnostic)diagnostic->knownBits|=1;

    std::array<AerRealGeometry,2> candidate{};
    std::array<double,2> scales{};
    bool baseline=true;
    bool seenMain=false,seenVrcam=false;
    // Relative tolerance covers the measured per-eye native projection drift
    // (at most 0.046%) without accepting anisotropic or one-eye zoom.
    constexpr double scaleTolerance=.001;
    constexpr double centerTolerance=.001;
    constexpr double minimumScale=.25,maximumScale=16.;
    const auto sameScale=[scaleTolerance](double a,double b){
        return std::abs(a-b)<=scaleTolerance*std::max(a,b);
    };
    for(unsigned eye=0;eye<2;++eye){
        const auto& stamp=frame.nativeStamp.eyes[eye];
        const auto& projection=frame.projection[eye];
        if(!projection)return reject(PairedGeometryReason::ProjectionMissing,eye);
        if(frame.lease->resources[eye]!=frame.color[eye])return reject(PairedGeometryReason::LeaseResource,eye);
        if(!stamp.sourceSerial)return reject(PairedGeometryReason::SourceSerialMissing,eye);
        if(stamp.sourceSerial!=frame.identity.serial)return reject(PairedGeometryReason::SourceSerialMismatch,eye);
        if(!stamp.poseId)return reject(PairedGeometryReason::PoseMissing,eye);
        if(!stamp.poseWriteId)return reject(PairedGeometryReason::PoseWriteMissing,eye);
        if(stamp.poseId!=frame.identity.pose[eye])return reject(PairedGeometryReason::PoseMismatch,eye);
        if(stamp.spaceEpoch!=frame.identity.origin)return reject(PairedGeometryReason::SpaceMismatch,eye);
        if(!stamp.nativeFrameKnown)return reject(PairedGeometryReason::NativeFrameUnknown,eye);
        if(!stamp.referenceOptics.valid)return reject(PairedGeometryReason::ReferenceOpticsInvalid,eye);
        if(frame.engineView[eye]!=stamp.engineView)return reject(PairedGeometryReason::EngineViewMismatch,eye);
        if(stamp.engineView!=1&&stamp.engineView!=2)return reject(PairedGeometryReason::EngineViewUnsupported,eye);
        if(projection->view!=stamp.engineView)return reject(PairedGeometryReason::ProjectionView,eye);
        if(projection->frameId!=stamp.nativeFrame)return reject(PairedGeometryReason::ProjectionFrame,eye);
        if(projection->poseId!=stamp.poseId)return reject(PairedGeometryReason::ProjectionPose,eye);
        if(projection->poseWriteId!=stamp.poseWriteId)return reject(PairedGeometryReason::ProjectionPoseWrite,eye);
        if(projection->origin!=stamp.spaceEpoch)return reject(PairedGeometryReason::ProjectionOrigin,eye);
        if(projection->clockGeneration!=stamp.clockGeneration)return reject(PairedGeometryReason::ProjectionClock,eye);
        if(projection->cameraApplicationQpc!=stamp.cameraApplicationQpc)return reject(PairedGeometryReason::ProjectionApplicationTime,eye);
        if(projection->contentTimeNs!=stamp.contentTimeNs)return reject(PairedGeometryReason::ProjectionContentTime,eye);
        if(projection->posePredictionNs!=stamp.posePredictionNs)return reject(PairedGeometryReason::ProjectionPredictionTime,eye);
        if(!projection->queue)return reject(PairedGeometryReason::ProjectionQueueMissing,eye);
        if(!SameDevice(projection->queue.Get(),frame.color[eye]))return reject(PairedGeometryReason::ProjectionColorDevice,eye);
        if(!SameDevice(projection->queue.Get(),frame.colorCompletion.fence.Get()))return reject(PairedGeometryReason::ProjectionFenceDevice,eye);
        known(eye,1);
        seenMain|=stamp.engineView==1;
        seenVrcam|=stamp.engineView==2;
        if(eye&&stamp.nativeFrame!=frame.nativeStamp.eyes[0].nativeFrame)return reject(PairedGeometryReason::PairNativeFrame,eye);

        const auto desc=frame.color[eye]->GetDesc();
        if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D)return reject(PairedGeometryReason::TextureDimension,eye);
        if(!desc.Width)return reject(PairedGeometryReason::WidthMissing,eye);
        if(!desc.Height)return reject(PairedGeometryReason::HeightMissing,eye);
        if(desc.Width>UINT32_MAX)return reject(PairedGeometryReason::WidthOverflow,eye);
        if(desc.Height>16384)return reject(PairedGeometryReason::HeightLimit,eye);
        if(desc.DepthOrArraySize!=1)return reject(PairedGeometryReason::ArraySize,eye);
        if(desc.MipLevels!=1)return reject(PairedGeometryReason::MipLevels,eye);
        if(desc.SampleDesc.Count!=1)return reject(PairedGeometryReason::SampleCount,eye);
        if(desc.SampleDesc.Quality)return reject(PairedGeometryReason::SampleQuality,eye);
        if(!Color(desc.Format,frame.nativeStamp.transfer))return reject(PairedGeometryReason::ColorFormat,eye);
        auto& geometry=candidate[eye];
        geometry.width=uint32_t(desc.Width);geometry.height=desc.Height;
        geometry.crop={0,0,float(geometry.width),float(geometry.height)};
        const auto& constants=projection->constants;
        if(diagnostic){diagnostic->eyes[eye].width=geometry.width;diagnostic->eyes[eye].height=geometry.height;}known(eye,2);
        if(!Projection(constants,geometry))return reject(PairedGeometryReason::ProjectionInvalid,eye);
        if(diagnostic){const auto& p=constants.values;diagnostic->eyes[eye].projection={p[0],p[5],p[8],p[9],p[11],p[14]};}known(eye,4);
        const auto& optics=stamp.referenceOptics;
        const auto fit=cvr::ppd::CalibrateReferenceOptics(geometry.width,geometry.height,optics);
        known(eye,8);
        if(!fit.valid)return reject(PairedGeometryReason::ReferenceFit,eye);
        const auto expected=cvr::ppd::ReferenceSourceFov(geometry.width,geometry.height,fit);
        if(diagnostic)diagnostic->eyes[eye].expectedFov={expected.angleLeft,expected.angleRight,expected.angleUp,expected.angleDown};known(eye,16);
        if(!cvr::ppd::ValidReferenceFov(expected))return reject(PairedGeometryReason::ExpectedFov,eye);
        const auto& p=constants.values;
        const double expectedX=1./std::tan(double(expected.angleRight));
        const double expectedY=1./std::tan(double(expected.angleUp));
        const double scaleX=double(p[0])/expectedX,scaleY=double(p[5])/expectedY;
        const double centerX=double(p[8])/double(p[11]);
        const double centerY=double(p[9])/double(p[11]);
        if(diagnostic){auto& d=diagnostic->eyes[eye];d.expectedX=expectedX;d.expectedY=expectedY;d.scaleX=scaleX;d.scaleY=scaleY;d.centerX=centerX;d.centerY=centerY;}known(eye,32);
        if(!std::isfinite(scaleX))return reject(PairedGeometryReason::ScaleXNotFinite,eye);
        if(!std::isfinite(scaleY))return reject(PairedGeometryReason::ScaleYNotFinite,eye);
        if(scaleX<minimumScale)return reject(PairedGeometryReason::ScaleXMinimum,eye);
        if(scaleX>maximumScale)return reject(PairedGeometryReason::ScaleXMaximum,eye);
        if(scaleY<minimumScale)return reject(PairedGeometryReason::ScaleYMinimum,eye);
        if(scaleY>maximumScale)return reject(PairedGeometryReason::ScaleYMaximum,eye);
        if(!sameScale(scaleX,scaleY))return reject(PairedGeometryReason::AnisotropicScale,eye);
        if(std::abs(centerX)>centerTolerance)return reject(PairedGeometryReason::CenterX,eye);
        if(std::abs(centerY)>centerTolerance)return reject(PairedGeometryReason::CenterY,eye);
        scales[eye]=(scaleX+scaleY)*.5;
        if(diagnostic)diagnostic->eyes[eye].computedScale=scales[eye];

        const auto crop=cvr::ppd::ReferenceEyeCrop(geometry.width,geometry.height,fit,optics.applied[eye]);
        if(diagnostic)diagnostic->eyes[eye].crop=crop;known(eye,64);
        if(crop[0]<0)return reject(PairedGeometryReason::CropLeft,eye);
        if(crop[1]<0)return reject(PairedGeometryReason::CropTop,eye);
        if(crop[2]<=crop[0])return reject(PairedGeometryReason::CropWidth,eye);
        if(crop[3]<=crop[1])return reject(PairedGeometryReason::CropHeight,eye);
        if(crop[2]>float(geometry.width))return reject(PairedGeometryReason::CropRight,eye);
        if(crop[3]>float(geometry.height))return reject(PairedGeometryReason::CropBottom,eye);
        geometry.crop=crop;
        const auto tangents=cvr::ppd::FovTangents(optics.applied[eye]);
        if(diagnostic)diagnostic->eyes[eye].tangents={tangents.up,tangents.down,tangents.left,tangents.right};known(eye,128);
        unsigned component=0;
        for(float value:{tangents.up,tangents.down,tangents.left,tangents.right}){
            if(!Number(value))return reject(PairedGeometryReason::TangentInvalid,eye,component);
            if(value<=0)return reject(PairedGeometryReason::TangentNonPositive,eye,component);
            ++component;
        }
        geometry.tangents={tangents.up,tangents.down,tangents.left,tangents.right};
        const auto& f=optics.applied[eye];
        geometry.effectiveFov={f.angleLeft,f.angleRight,f.angleUp,f.angleDown};
        AerRealGeometry strict;strict.width=geometry.width;strict.height=geometry.height;
        baseline&=ResolveGeometry(constants,optics,eye,{},strict);
    }
    if(!seenMain)return reject(PairedGeometryReason::MainViewMissing);
    if(!seenVrcam)return reject(PairedGeometryReason::VrcamViewMissing);
    if(!sameScale(scales[0],scales[1]))return reject(PairedGeometryReason::PairScale);
    output=candidate;
    baselineCalibration=baseline;
    if(diagnostic){diagnostic->reason=PairedGeometryReason::Ready;diagnostic->knownBits|=1u<<18;}
    return true;
}
AerRealCandidateResult AerRealPresenter::FromMainFrame(Frame frame,AerV2Eye physicalEye,std::optional<std::array<float,4>> crop){
    AerRealCandidateResult out;const auto eye=uint32_t(physicalEye);
    if(eye>1||!frame.color[eye]||frame.color[eye^1]||!frame.identity.serial||!frame.identity.origin)return out;
    const auto&stamp=frame.nativeStamp.eyes[eye];const auto&receipt=stamp.mainEyeSource;
    if(!receipt||receipt.physicalEye!=physicalEye||frame.engineView[eye]!=1||stamp.engineView!=1||
       !stamp.sourceSerial||stamp.sourceSerial!=frame.identity.serial||!stamp.poseId||!stamp.poseWriteId||
       frame.identity.pose[eye]!=stamp.poseId||!stamp.nativeFrameKnown||!frame.nativeStamp.resourceEpoch||
       !stamp.clockGeneration||stamp.spaceEpoch!=frame.identity.origin||stamp.spaceEpoch!=receipt.epochs.hmdWorld||
       stamp.contentTimeNs<=0||stamp.cameraApplicationQpc<=0||!receipt.MatchesWrittenPose(stamp.spaceEpoch,stamp.posePredictionNs))return out;
    if(!frame.lease||!frame.lease->owner||!frame.lease->release||frame.lease->resources[eye]!=frame.color[eye]||frame.lease->resources[eye^1]){
        out.code=AerRealCode::MissingSourceLease;return out;
    }
    out.code=Completion(frame);if(out.code!=AerRealCode::Ready)return out;
    if(!SameDevice(frame.color[eye],frame.colorCompletion.fence.Get())){out.code=AerRealCode::InvalidInput;return out;}
    const auto desc=frame.color[eye]->GetDesc();
    if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||!desc.Width||!desc.Height||desc.Width>UINT32_MAX||
       desc.DepthOrArraySize!=1||desc.SampleDesc.Count!=1||desc.SampleDesc.Quality||!Color(desc.Format,frame.nativeStamp.transfer)){
        out.code=AerRealCode::InvalidInput;return out;
    }
    AerRealGeometry geometry;const auto&h=stamp.capturedHead;
    geometry.capturedHead={{h[0],h[1],h[2],h[3]},{h[4],h[5],h[6]}};
    if(!stamp.capturedHeadValid||!Pose(geometry.capturedHead)||!Pose(frame.pose[eye])){out.code=AerRealCode::MissingCapturedHead;return out;}
    geometry.eyeFromHead=RelativePose(geometry.capturedHead,frame.pose[eye]);
    if(!ParallelEye(geometry.eyeFromHead)){out.code=AerRealCode::UnsupportedCant;return out;}
    if(!frame.projection[eye]&&frame.inputs[eye])frame.projection[eye]=SnapshotFrameProjection(*frame.inputs[eye]);
    // AER geometry/PPD must not reserve one of the native four guide Records.
    // A present but invalid snapshot is rejected, never replaced by a fallback.
    for(auto& input:frame.inputs)input.reset();
    frame.identity.inputs=false;
    const auto&input=frame.projection[eye];
    if(!input||input->view!=1||input->frameId!=stamp.nativeFrame||input->poseId!=stamp.poseId||input->poseWriteId!=stamp.poseWriteId||
       input->origin!=stamp.spaceEpoch||input->clockGeneration!=stamp.clockGeneration||input->cameraApplicationQpc!=stamp.cameraApplicationQpc||
       input->contentTimeNs!=stamp.contentTimeNs||input->posePredictionNs!=stamp.posePredictionNs||!input->queue||!SameDevice(input->queue.Get(),frame.color[eye])){
        out.code=AerRealCode::UnsupportedProjection;return out;
    }
    geometry.width=uint32_t(desc.Width);geometry.height=desc.Height;
    if(!ResolveGeometry(input->constants,stamp.referenceOptics,eye,crop,geometry)){
        out.code=AerRealCode::UnsupportedProjection;return out;
    }
    AerRealCandidate candidate;candidate.receipt=receipt;candidate.geometry=geometry;
    candidate.colorOwner=frame.color[eye];candidate.producerQueue=input->queue;
    frame.identity.reset=frame.identity.reset||input->reset;
    // MFG receives full-image intrinsics; only XR and warp use the cropped FOV.
    frame.fov[eye]=geometry.renderedFov;candidate.frame=std::move(frame);
    out.code=AerRealCode::Ready;out.candidate=std::move(candidate);return out;
}

bool AerRealPresenter::SameCalibration(const AerRealGeometry&a,const AerRealGeometry&b){
    if(!a.width||!a.height||a.width!=b.width||a.height!=b.height||!Pose(a.eyeFromHead)||!Pose(b.eyeFromHead))return false;
    for(unsigned i=0;i<4;++i)if(!Number(a.crop[i])||!Number(b.crop[i])||std::abs(a.crop[i]-b.crop[i])>CalibrationCropTolerance)return false;
    const auto fovClose=[](const XrFovf&x,const XrFovf&y){
        const std::array<float,4> av{x.angleLeft,x.angleRight,x.angleUp,x.angleDown},bv{y.angleLeft,y.angleRight,y.angleUp,y.angleDown};
        for(unsigned i=0;i<4;++i)if(!Number(av[i])||!Number(bv[i])||std::abs(av[i]-bv[i])>CalibrationAngularTolerance)return false;
        return true;
    };
    if(!fovClose(a.renderedFov,b.renderedFov)||!fovClose(a.effectiveFov,b.effectiveFov))return false;
    const auto&x=a.eyeFromHead;const auto&y=b.eyeFromHead;
    const float dx=x.position.x-y.position.x,dy=x.position.y-y.position.y,dz=x.position.z-y.position.z;
    if(dx*dx+dy*dy+dz*dz>CalibrationPositionTolerance*CalibrationPositionTolerance)return false;
    const std::array<float,4> qa{x.orientation.x,x.orientation.y,x.orientation.z,x.orientation.w},qb{y.orientation.x,y.orientation.y,y.orientation.z,y.orientation.w};
    float minus=0,plus=0;for(unsigned i=0;i<4;++i){minus+=(qa[i]-qb[i])*(qa[i]-qb[i]);plus+=(qa[i]+qb[i])*(qa[i]+qb[i]);}
    const float chord=2*std::sin(CalibrationAngularTolerance*.25f);
    return std::min(minus,plus)<=chord*chord;
}
void AerRealPresenter::ClearSources(bool clearObservedCalibration){
    pending_={};eyes_={};policy_.Reset();referenceHead_={};referenceReceipt_={};
    resourceEpoch_=requestRevision_=latchRevision_=sourceClockGeneration_=captureClockGeneration_=0;
    calibrationSourceFloor_=-1;
    // Last accepted signatures are plain geometry, not source owners. Keeping
    // them across epochs lets a real resize advance calibrationRevision while
    // a recenter/world-position change alone does not.
    if(clearObservedCalibration)observedCalibration_={};
    if(sourceGeneration_!=UINT64_MAX)++sourceGeneration_;
}
void AerRealPresenter::Reset(){ClearSources();generation_=0;epochs_={};}
void AerRealPresenter::ConstrainSourceScope(const MainEyeSourceReceipt& current,uint64_t captureClockGeneration){
    if(!current || !captureClockGeneration ||
       (resourceEpoch_ && (current.epochs!=epochs_ || current.requestRevision!=requestRevision_ ||
        current.latchRevision!=latchRevision_ || current.clockGeneration!=sourceClockGeneration_ ||
        captureClockGeneration!=captureClockGeneration_)))ClearSources();
}
uint32_t AerRealPresenter::HeldEyeCount()const{return unsigned(bool(eyes_[0]))+unsigned(bool(eyes_[1]));}
uint32_t AerRealPresenter::QueuedEyeCount(uint32_t eye)const{
    if(eye>1)return 0;uint32_t count{};
    for(size_t i=0;i<SourceSlotsPerEye;++i)count+=bool(pending_[eye*SourceSlotsPerEye+i]);
    return count;
}
std::shared_ptr<void> AerRealSelection::OwnerForEye(uint32_t i)const{return i<2?std::const_pointer_cast<AerRealEye>(eyes[i]):nullptr;}
AerRealSelection AerRealPresenter::Step(const AerRealTick&tick,std::optional<AerRealCandidate> candidate){
    if(!candidate)return StepReady(tick,{});
    std::array<AerRealCandidate,1> ready{std::move(*candidate)};
    return StepReady(tick,ready);
}
AerRealSelection AerRealPresenter::StepReady(const AerRealTick&tick,std::span<AerRealCandidate> ready,const AerReadyObserver& observer){
    AerRealSelection out;
    if(ready.size()>MaxReadyCandidates||!tick.generation||!tick.epochs.Valid()||tick.frame<0||tick.appliedDelay<0||tick.sourceUpper< -1||
       (generation_==tick.generation&&epochs_==tick.epochs&&tick.frame<=policy_.State().lastFrame)){
        out.code=AerRealCode::InvalidInput;return out;
    }
    if(generation_!=tick.generation||epochs_!=tick.epochs){ClearSources();generation_=tick.generation;epochs_=tick.epochs;}
    constexpr size_t BatchCapacity=MaxReadyCandidates+SourceSlotsPerEye*2;
    std::array<std::optional<AerRealCandidate>,BatchCapacity> candidates;
    for(size_t i=0;i<pending_.size();++i){candidates[MaxReadyCandidates+i]=std::move(pending_[i]);pending_[i].reset();}
    for(size_t i=0;i<ready.size();++i){
        auto candidate=std::move(ready[i]);ready[i]={};
        if(candidate.receipt.epochs!=epochs_){out.candidateCode=AerRealCode::EpochMismatch;continue;}
        auto rebuilt=FromMainFrame(std::move(candidate.frame),candidate.receipt.physicalEye,candidate.geometry.crop);
        out.candidateCode=rebuilt.code;
        if(!rebuilt)continue;
        if(rebuilt.candidate->receipt.epochs!=epochs_){out.candidateCode=AerRealCode::EpochMismatch;continue;}
        candidates[i]=std::move(rebuilt.candidate);
    }
    using Domain=std::array<uint64_t,5>;
    const auto domainOf=[](const AerRealCandidate& candidate)->Domain{
        const auto&r=candidate.receipt;
        return {candidate.frame.nativeStamp.resourceEpoch,r.requestRevision,r.latchRevision,r.clockGeneration,
                candidate.frame.nativeStamp.eyes[uint32_t(r.physicalEye)].clockGeneration};
    };
    const auto atLeast=[](const Domain&a,const Domain&b){
        for(size_t i=0;i<a.size();++i)if(a[i]<b[i])return false;return true;
    };
    const Domain current{resourceEpoch_,requestRevision_,latchRevision_,sourceClockGeneration_,captureClockGeneration_};
    for(auto& candidate:candidates)if(candidate&&resourceEpoch_&&!atLeast(domainOf(*candidate),current)){
        out.candidateCode=AerRealCode::EpochMismatch;candidate.reset();
    }
    // Find one authoritative monotonic domain for the entire tick. A batch of
    // incomparable clocks is rejected rather than choosing a scope by eye or
    // mutating policy halfway through its R/L callbacks.
    std::optional<Domain> selectedDomain;
    for(const auto& candidate:candidates)if(candidate){
        const auto domain=domainOf(*candidate);bool dominates=true;
        for(const auto& other:candidates)if(other&&!atLeast(domain,domainOf(*other))){dominates=false;break;}
        if(dominates){selectedDomain=domain;break;}
    }
    if(selectedDomain){
        for(auto& candidate:candidates)if(candidate&&domainOf(*candidate)!=*selectedDomain){
            out.candidateCode=AerRealCode::EpochMismatch;candidate.reset();
        }
        if(resourceEpoch_&&current!=*selectedDomain)ClearSources();
        resourceEpoch_=(*selectedDomain)[0];requestRevision_=(*selectedDomain)[1];latchRevision_=(*selectedDomain)[2];
        sourceClockGeneration_=(*selectedDomain)[3];captureClockGeneration_=(*selectedDomain)[4];
    }else{
        for(auto& candidate:candidates)if(candidate){out.candidateCode=AerRealCode::EpochMismatch;candidate.reset();}
    }
    // A frame already queued is immutable. Re-reading its catalogue cell (or
    // another desktop capture of that same game source) must not replace it.
    for(size_t i=MaxReadyCandidates;i<candidates.size();++i)if(candidates[i])
        for(size_t j=0;j<MaxReadyCandidates;++j)if(candidates[j]&&
            candidates[j]->receipt.sourceId==candidates[i]->receipt.sourceId&&
            candidates[j]->receipt.physicalEye==candidates[i]->receipt.physicalEye)candidates[j].reset();
    // Calibration is a preflight decision, independent of callback order.
    // Only the newest ready geometry for each eye defines this batch's route.
    std::array<AerRealCandidate*,2> newest{};
    for(auto& candidate:candidates)if(candidate){
        const auto eye=uint32_t(candidate->receipt.physicalEye);auto*& selected=newest[eye];
        if(candidate->receipt.sourceId<policy_.State().sourceIds[eye]){out.candidateCode=AerRealCode::Duplicate;candidate.reset();continue;}
        if(candidate->receipt.sourceId<calibrationSourceFloor_){out.candidateCode=AerRealCode::Ineligible;candidate.reset();continue;}
        if(!selected||candidate->receipt.sourceId>selected->receipt.sourceId||
           (candidate->receipt.sourceId==selected->receipt.sourceId&&candidate->frame.identity.serial>selected->frame.identity.serial))selected=&*candidate;
    }
    bool calibrationChanged=false;int64_t freshFloor=(std::numeric_limits<int64_t>::max)();
    for(unsigned eye=0;eye<2;++eye)if(newest[eye]&&acceptedCalibration_[eye]){
        const auto& comparison=observedCalibration_[eye]?*observedCalibration_[eye]:*acceptedCalibration_[eye];
        if(!SameCalibration(comparison,newest[eye]->geometry)){
            observedCalibration_[eye]=newest[eye]->geometry;calibrationChanged=true;
            freshFloor=std::min(freshFloor,newest[eye]->receipt.sourceId);
        }
    }
    if(calibrationChanged){
        const auto priorFloor=calibrationSourceFloor_;
        if(calibrationRevision_!=UINT64_MAX)++calibrationRevision_;
        ClearSources(false);
        calibrationSourceFloor_=std::max(priorFloor,freshFloor);
        // ClearSources invalidates the old pair, not this validated new domain.
        resourceEpoch_=(*selectedDomain)[0];requestRevision_=(*selectedDomain)[1];latchRevision_=(*selectedDomain)[2];
        sourceClockGeneration_=(*selectedDomain)[3];captureClockGeneration_=(*selectedDomain)[4];
    }
    std::array<std::optional<AerRealGeometry>,2> batchGeometry;
    for(unsigned eye=0;eye<2;++eye)if(newest[eye])batchGeometry[eye]=newest[eye]->geometry;
    for(auto& candidate:candidates)if(candidate){
        const auto eye=uint32_t(candidate->receipt.physicalEye);
        if(candidate->receipt.sourceId<calibrationSourceFloor_||!batchGeometry[eye]||!SameCalibration(*batchGeometry[eye],candidate->geometry)){
            out.candidateCode=AerRealCode::Ineligible;candidate.reset();continue;
        }
        out.candidateCode=candidate->receipt.sourceId<=policy_.State().sourceIds[eye]?AerRealCode::Duplicate:AerRealCode::Ineligible;
    }
    if(observer){
        std::array<const AerRealCandidate*,BatchCapacity> ordered{};
        size_t count=0;
        for(auto& candidate:candidates)if(candidate){
            const auto completion=Completion(candidate->frame);
            if(completion!=AerRealCode::Ready){out.candidateCode=completion;candidate.reset();continue;}
            ordered[count++]=&*candidate;
        }
        if(count){
            std::sort(ordered.begin(),ordered.begin()+count,[](const auto* a,const auto* b){
                if(a->receipt.sourceId!=b->receipt.sourceId)return a->receipt.sourceId<b->receipt.sourceId;
                return a->frame.identity.serial>b->frame.identity.serial;
            });
            size_t unique=0;
            for(size_t i=0;i<count;++i)if(!unique||ordered[i]->receipt.sourceId!=ordered[unique-1]->receipt.sourceId)
                ordered[unique++]=ordered[i];
            const AerReadyIngress ingress{generation_,sourceGeneration_,calibrationRevision_,resourceEpoch_,requestRevision_,latchRevision_,
                sourceClockGeneration_,captureClockGeneration_,epochs_,{ordered.data(),unique}};
            // Borrowed view only; all preflight/reset decisions are complete.
            // An exception unwinds candidates before any policy admission.
            try{observer(ingress);}catch(...){
                // Optional downstream ingestion must not silently consume the
                // previously queued baseline images when it throws.
                for(size_t i=MaxReadyCandidates;i<candidates.size();++i)
                    pending_[i-MaxReadyCandidates]=std::move(candidates[i]);
                throw;
            }
        }
    }
    out.policyStep=policy_.Step(tick.frame,tick.appliedDelay,tick.sourceUpper,[&](const AerV2Request&request)->std::optional<AerV2Admission>{
        std::optional<AerRealCandidate>* selected=nullptr;
        for(auto& candidate:candidates)if(candidate&&candidate->receipt.physicalEye==request.eye){
            // Original307630 retires ready FIFO heads below lower even when
            // no new source can be admitted. Keeping them would occupy all
            // three producer cells after the opposite eye skips source IDs.
            if(candidate->receipt.sourceId<request.lowerSource){
                out.candidateCode=candidate->receipt.sourceId<=policy_.State().sourceIds[uint32_t(request.eye)]?
                    AerRealCode::Duplicate:AerRealCode::Ineligible;
                candidate.reset();continue;
            }
            if(candidate->receipt.sourceId>request.upperSource)continue;
            const auto completion=Completion(candidate->frame);
            if(completion!=AerRealCode::Ready){out.candidateCode=completion;candidate.reset();continue;}
            if(!selected||candidate->receipt.sourceId<(*selected)->receipt.sourceId||
               (candidate->receipt.sourceId==(*selected)->receipt.sourceId&&candidate->frame.identity.serial>(*selected)->frame.identity.serial)){
                selected=&candidate;
            }
        }
        if(!selected)return {};
        auto& candidate=*selected;
        const auto eye=uint32_t(request.eye);const auto id=candidate->receipt.sourceId;
        auto owner=std::make_shared<AerRealEye>();owner->frame=std::move(candidate->frame);owner->receipt=candidate->receipt;owner->geometry=candidate->geometry;
        owner->colorOwner=std::move(candidate->colorOwner);owner->producerQueue=std::move(candidate->producerQueue);
        eyes_[eye]=owner;referenceHead_=owner->geometry.capturedHead;referenceReceipt_=owner->receipt;
        acceptedCalibration_[eye]=owner->geometry;observedCalibration_[eye].reset();
        candidate.reset();out.candidateCode=AerRealCode::Ready;return AerV2Admission{request.eye,id,true};
    });
    // Original307630 leaves later sources in their FIFO, not in a replaceable
    // latest-image mailbox. Retain at most three slots per eye, including its
    // held image. Only completed, validated sources may enter this queue.
    for(uint32_t eye=0;eye<2;++eye){
        const size_t limit=SourceSlotsPerEye-unsigned(bool(eyes_[eye]));
        for(size_t slot=0;slot<limit;++slot){
            std::optional<AerRealCandidate>* next=nullptr;
            for(auto& candidate:candidates)if(candidate&&uint32_t(candidate->receipt.physicalEye)==eye){
                if(candidate->receipt.sourceId<=policy_.State().sourceIds[eye]){candidate.reset();continue;}
                if(Completion(candidate->frame)!=AerRealCode::Ready){candidate.reset();continue;}
                if(!next||candidate->receipt.sourceId<(*next)->receipt.sourceId||
                   (candidate->receipt.sourceId==(*next)->receipt.sourceId&&candidate->frame.identity.serial>(*next)->frame.identity.serial))next=&candidate;
            }
            if(!next)break;
            const auto id=(*next)->receipt.sourceId;
            pending_[eye*SourceSlotsPerEye+slot]=std::move(*next);next->reset();
            for(auto& candidate:candidates)if(candidate&&uint32_t(candidate->receipt.physicalEye)==eye&&candidate->receipt.sourceId==id)candidate.reset();
        }
    }
    for(auto& candidate:candidates)candidate.reset();
    out.generation=generation_;out.sourceGeneration=sourceGeneration_;out.calibrationRevision=calibrationRevision_;out.epochs=epochs_;
    out.resourceEpoch=resourceEpoch_;out.sourceClockGeneration=sourceClockGeneration_;out.captureClockGeneration=captureClockGeneration_;
    out.referenceHead=referenceHead_;out.referenceReceipt=referenceReceipt_;out.newlyAccepted=out.policyStep.accepted;out.eyes=eyes_;
    if(out.policyStep.status!=AerV2StepStatus::Advanced){out.code=AerRealCode::InvalidInput;return out;}
    bool complete=bool(referenceReceipt_);
    for(unsigned eye=0;eye<2;++eye){
        const auto&owner=eyes_[eye];if(!owner){complete=false;continue;}
        const auto completion=Completion(owner->frame);if(completion!=AerRealCode::Ready){out.code=completion;return out;}
        out.sourceIds[eye]=owner->receipt.sourceId;out.fov[eye]=owner->geometry.effectiveFov;
        // 2A0B80/87 also copies reference XYZ to submit metadata. This is the
        // proven REAL D1 behavior; it does not claim translational pixel warp.
        out.submitPose[eye]=Compose(referenceHead_,owner->geometry.eyeFromHead);
        auto&w=out.warp[eye];const auto&q=owner->geometry.capturedHead.orientation;const auto&r=referenceHead_.orientation;
        w.captured={q.x,q.y,q.z,q.w};w.reference={r.x,r.y,r.z,r.w};w.crop=owner->geometry.crop;w.tangents=owner->geometry.tangents;
        // Validated calibration domain, not the bridge's generation epoch.
        w.sourceEpoch=w.referenceEpoch=sourceGeneration_;w.sourceSpace=w.referenceSpace=epochs_.hmdWorld;
    }
    out.code=complete?AerRealCode::Ready:AerRealCode::NotReady;return out;
}

std::optional<AerNativePair> BuildAerNativePair(const AerRealSelection&baseline,
        const std::shared_ptr<NativeSelectionLease>&lease,uint64_t currentBridgeEpoch){
    if(!baseline||!lease||!lease->device||!lease->selected.frame||!currentBridgeEpoch||
       !baseline.generation||!baseline.sourceGeneration||!baseline.resourceEpoch||!baseline.epochs.Valid()||
       !Pose(baseline.referenceHead)||!baseline.referenceReceipt||baseline.referenceReceipt.epochs!=baseline.epochs||
       baseline.referenceReceipt.clockGeneration!=baseline.sourceClockGeneration)return {};
    const auto&pair=*lease->selected.frame;
    if(!pair.pair_id||pair.epoch!=currentBridgeEpoch||pair.space_epoch!=baseline.epochs.hmdWorld||
       pair.cadence!=cpvr::Cadence::AlternateEyes)return {};
    AerNativePair out;out.owner=lease;out.pairId=pair.pair_id;out.bridgeEpoch=currentBridgeEpoch;
    out.generation=baseline.generation;out.sourceGeneration=baseline.sourceGeneration;out.allReal=true;
    const auto family=[](DXGI_FORMAT format){switch(format){
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:return 1;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:case DXGI_FORMAT_B8G8R8A8_UNORM:case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:return 2;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:return 3;case DXGI_FORMAT_R32G32B32A32_FLOAT:return 4;default:return 0;}};
    for(unsigned eye=0;eye<2;++eye){
        const auto&base=baseline.eyes[eye];const auto&value=pair.eyes[eye];
        if(!base||!value||value->eye!=cpvr::Eye(eye)||value->epoch!=pair.epoch||value->space_epoch!=pair.space_epoch||
           !value->source_id||value->stereo_pair_id||value->content_time_ns<=0||value->source_id>uint64_t(SourceDisplayClock::MaxExactSourceId)+1||
           ((value->source_id-1)&1)!=eye||value->source_id>uint64_t(base->receipt.sourceId)+1||
           base->receipt.epochs!=baseline.epochs||base->receipt.sourceId!=baseline.sourceIds[eye]||
           base->frame.nativeStamp.resourceEpoch!=baseline.resourceEpoch)return {};
        const auto&image=value->color;
        if(!image.texture||!image.lifetime||!image.ready_fence||!image.ready_value||image.ready_value==UINT64_MAX||
           !SameDevice(image.texture.Get(),image.ready_fence.Get())||!SameDevice(image.texture.Get(),base->frame.color[eye])||
           (value->generated&&image.texture.Get()==base->frame.color[eye]))return {};
        Microsoft::WRL::ComPtr<ID3D12Device> actual;
        Microsoft::WRL::ComPtr<IUnknown> actualId,leaseId;
        if(FAILED(image.texture->GetDevice(IID_PPV_ARGS(&actual)))||FAILED(actual.As(&actualId))||
           FAILED(lease->device.As(&leaseId))||actualId.Get()!=leaseId.Get())return {};
        const auto completed=image.ready_fence->GetCompletedValue();if(completed==UINT64_MAX||completed<image.ready_value)return {};
        const auto desc=image.texture->GetDesc(),original=base->frame.color[eye]->GetDesc();
        const auto&g=base->geometry;
        if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||desc.DepthOrArraySize!=1||desc.SampleDesc.Count!=1||desc.SampleDesc.Quality||
           desc.Width!=original.Width||desc.Height!=original.Height||!family(desc.Format)||family(desc.Format)!=family(original.Format)||
           g.width!=original.Width||g.height!=original.Height||
           !ParallelEye(g.eyeFromHead))return {};
        // NVIDIA consumes the full game image. Its R/G outputs must use the
        // same validated eye region and rays as the corresponding real image.
        // Recompute from native intrinsics so a stale/forged crop cannot change
        // the projection merely by altering cached geometry metadata.
        if(!base->frame.projection[eye])return {};
        auto mapped=g;
        if(!ResolveGeometry(base->frame.projection[eye]->constants,
                base->frame.nativeStamp.eyes[eye].referenceOptics,eye,g.crop,mapped)||
           !AerRealPresenter::SameCalibration(g,mapped))return {};
        for(unsigned i=0;i<4;++i)if(!Number(g.tangents[i])||std::abs(g.tangents[i]-mapped.tangents[i])>.00001f)return {};
        const auto transfer=image.transfer==cpvr::ColorTransfer::SrgbEncoded?CaptureTransfer::SrgbEncoded:
            image.transfer==cpvr::ColorTransfer::Linear?CaptureTransfer::Linear:CaptureTransfer::Unknown;
        if(!Color(desc.Format,transfer)||transfer!=base->frame.nativeStamp.transfer)return {};
        const auto state=image.resting_state;
        if(value->generated?(state!=D3D12_RESOURCE_STATE_COMMON&&state!=D3D12_RESOURCE_STATE_COPY_SOURCE):state!=CaptureColorReadState)return {};
        const auto&f=value->fov;const auto&b=g.renderedFov;
        if(!Number(f.left)||!Number(f.right)||!Number(f.up)||!Number(f.down)||
           std::abs(f.left-b.angleLeft)>.0001f||std::abs(f.right-b.angleRight)>.0001f||
           std::abs(f.up-b.angleUp)>.0001f||std::abs(f.down-b.angleDown)>.0001f)return {};
        const auto&origin=lease->selected.origins[eye];
        if(origin.generated!=value->generated||origin.current_source_id!=value->source_id||
           origin.previous_native_pair_id||origin.current_native_pair_id||!origin.previous_source_id||
           ((origin.previous_source_id-1)&1)!=eye||origin.current_time_ns>base->frame.nativeStamp.eyes[eye].contentTimeNs||
           (origin.current_source_id==uint64_t(base->receipt.sourceId)+1&&
            origin.current_time_ns!=base->frame.nativeStamp.eyes[eye].contentTimeNs))return {};
        if(value->generated){
            if(origin.current_source_id<=origin.previous_source_id||origin.current_source_id-origin.previous_source_id!=2||
               origin.previous_time_ns<=0||origin.current_time_ns<=origin.previous_time_ns||
               value->content_time_ns<=origin.previous_time_ns||value->content_time_ns>=origin.current_time_ns||
               !origin.index||!origin.count||origin.count>5||origin.index>origin.count)return {};
            // Same overflow-safe, nearest-integer phase_time as the existing
            // native worker. Unknown or mismatched fractional provenance fails.
            const auto delta=origin.current_time_ns-origin.previous_time_ns;
            const int64_t divisor=int64_t(origin.count)+1;
            const auto expectedTime=origin.previous_time_ns+(delta/divisor)*origin.index+
                ((delta%divisor)*origin.index+divisor/2)/divisor;
            if(expectedTime!=value->content_time_ns)return {};
            const double previous=double(origin.previous_source_id-1),current=double(origin.current_source_id-1);
            out.sourcePositions[eye]=previous+double(origin.index)/double(divisor)*double(origin.current_source_id-origin.previous_source_id);
            if(!std::isfinite(out.sourcePositions[eye])||out.sourcePositions[eye]<=previous||out.sourcePositions[eye]>=current)return {};
            out.allReal=false;
        }else if(origin.previous_source_id!=origin.current_source_id||origin.index||origin.count||
                 origin.previous_time_ns!=value->content_time_ns||origin.current_time_ns!=value->content_time_ns)return {};
        else out.sourcePositions[eye]=double(value->source_id-1);
        const auto&p=value->render_pose;
        XrPosef rendered{{p.orientation[0],p.orientation[1],p.orientation[2],p.orientation[3]},
            {p.position[0],p.position[1],p.position[2]}};
        if(!Pose(rendered))return {};
        const XrPosef identity{{0,0,0,1},{0,0,0}};
        const auto inverseOffset=RelativePose(g.eyeFromHead,identity);
        const auto head=Compose(rendered,inverseOffset);
        if(!Pose(head))return {};
        auto outputHead=head;outputHead.orientation=baseline.referenceHead.orientation;
        // NVIDIA path is deliberately source-position preserving: its head
        // position comes from this product, not the latest REAL reference XYZ.
        // Both R and G in a mixed native pair follow this same rule.
        out.submitPose[eye]=Compose(outputHead,g.eyeFromHead);
        out.color[eye]=image.texture.Get();out.transfer[eye]=transfer;out.generated[eye]=value->generated;
        out.contentTimeNs[eye]=value->content_time_ns;out.fov[eye]=mapped.effectiveFov;
        auto&w=out.warp[eye];const auto&q=head.orientation;const auto&r=baseline.referenceHead.orientation;
        w.captured={q.x,q.y,q.z,q.w};w.reference={r.x,r.y,r.z,r.w};
        w.crop=mapped.crop;w.tangents=mapped.tangents;
        w.sourceEpoch=w.referenceEpoch=baseline.sourceGeneration;w.sourceSpace=w.referenceSpace=pair.space_epoch;
    }
    if(out.allReal)for(unsigned eye=0;eye<2;++eye)out.aerSourceIds[eye]=int64_t(pair.eyes[eye]->source_id-1);
    return out;
}
} // namespace cvr::framegen

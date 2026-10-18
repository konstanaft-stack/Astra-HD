#include "Framegen/NativeFgPairedSource.hpp"
#include "Framegen/PairedImageContract.hpp"
#include <atomic>
namespace cvr::framegen {
namespace {std::atomic<std::shared_ptr<const NativeFgPairedSource>> latest;}
SrSourceStamp PairedNativeStamp(const NativeEyeStamp& eye){
    SrSourceStamp out;out.poseId=eye.poseId;out.writeId=eye.poseWriteId;out.origin=eye.spaceEpoch;
    out.captureClockGeneration=eye.clockGeneration;out.locatedPoseTimeNs=eye.posePredictionNs;
    out.nativeFrame=eye.nativeFrame;out.nativeFrameKnown=eye.nativeFrameKnown;out.engineView=eye.engineView;
    out.poseKnown=eye.poseId&&eye.poseWriteId&&eye.spaceEpoch;return out;
}
bool MatchesPairedNativeStamp(const NativeEyeStamp&a,const SrSourceStamp&b){
    return !a.mainEyeSource&&!b.source&&b.poseKnown&&a.nativeFrameKnown&&b.nativeFrameKnown&&
        a.poseId&&a.poseId==b.poseId&&a.poseWriteId&&a.poseWriteId==b.writeId&&a.spaceEpoch&&a.spaceEpoch==b.origin&&
        a.clockGeneration&&a.clockGeneration==b.captureClockGeneration&&a.nativeFrame==b.nativeFrame&&
        a.engineView==b.engineView&&a.posePredictionNs>0&&a.posePredictionNs==b.locatedPoseTimeNs;
}
bool ValidNativeFgPairedSource(const NativeFgPairedSource&s){
    const auto& f=s.frame;
    if(!s.gameWorldEpoch||s.mainEye>1||f.engineView[s.mainEye]!=1||f.engineView[s.mainEye^1]!=2||!f.lease||
       f.nativeStamp.renderPublicationTimeNs<=0||!f.nativeStamp.publicationClockGeneration||
       !f.identity.stereo||!f.identity.inputs||!f.inputs[0]||!f.inputs[1]||f.nativeStamp.transfer!=CaptureTransfer::SrgbEncoded)return false;
    PairedImageIdentity id;id.serial=f.identity.serial;id.origin=f.identity.origin;id.resourceEpoch=f.nativeStamp.resourceEpoch;
    id.producerSignalKnown=f.colorCompletion.HasScheduledCopySource();
    for(unsigned eye=0;eye<2;++eye){const auto& a=f.nativeStamp.eyes[eye];const auto& g=f.inputs[eye];
        id.eyes[eye]={uintptr_t(f.color[eye]),a.sourceSerial,a.poseId,a.poseWriteId,a.spaceEpoch,a.engineView,a.nativeFrame,a.nativeFrameKnown};
        if(a.mainEyeSource||!g||!g->submitted||!g->evaluated||!g->hasDepth||!g->hasMotion||!g->depth||!g->motion||!g->readyFence||!g->fenceValue||
           !g->queue||g->queue!=f.inputs[eye^1]->queue||f.lease->resources[eye]!=f.color[eye]||!f.projection[eye]||
           g->frameId!=a.nativeFrame||g->poseId!=a.poseId||g->poseWriteId!=a.poseWriteId||g->origin!=a.spaceEpoch||
           g->view!=a.engineView||g->clockGeneration!=a.clockGeneration||g->contentTimeNs<=0||g->contentTimeNs!=a.contentTimeNs||
           g->cameraApplicationQpc!=a.cameraApplicationQpc||g->posePredictionNs!=a.posePredictionNs)return false;
    }
    return ValidatePairedImage(id)==PairedImageAdmission::Ready;
}
void PublishNativeFgPairedSource(std::shared_ptr<const NativeFgPairedSource> source){
    if(source&&!ValidNativeFgPairedSource(*source))return;
    latest.store(std::move(source),std::memory_order_release);
}
std::shared_ptr<const NativeFgPairedSource> AcquireNativeFgPairedSource(const SrSourceStamp& main){
    auto value=latest.load(std::memory_order_acquire);
    return value&&value->mainEye<2&&main.engineView==1&&MatchesPairedNativeStamp(value->frame.nativeStamp.eyes[value->mainEye],main)?value:nullptr;
}
void ClearNativeFgPairedSource(){latest.store({},std::memory_order_release);}
}

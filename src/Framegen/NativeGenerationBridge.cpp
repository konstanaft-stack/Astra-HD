#include "Framegen/NativeGenerationBridge.hpp"
#include "Framegen/AerRealPresenter.hpp"
#include "Render/NativeObjectIdentity.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <deque>

namespace cvr::framegen {
bool OwnSelectionWitnessEnabled()noexcept{static const bool enabled=[] {char flag[8]{};return GetEnvironmentVariableA("CPVR_FG_HISTORY_DIAG",flag,sizeof(flag))==1&&flag[0]=='1';}();return enabled;}
uint64_t NextOwnSelectionAttempt()noexcept{static std::atomic<uint64_t> next{};return next.fetch_add(1,std::memory_order_relaxed)+1;}
namespace {
OwnWitnessCounters WitnessCounters(const cpvr::GenerationStatus& s,int64_t target){
    OwnWitnessCounters out;out.coherent=s.selection_target_ns==target&&!s.history_reset_pending;
    out.acceptedInputs=s.accepted_inputs;out.completedBatches=s.completed_batches;out.generatedOutputs=s.generated_outputs;
    out.selectedPairs=s.selected_pairs;out.generatedSelectedPairs=s.generated_selected_pairs;out.maximumAge=s.active_policy.maximum_age_ns;
    const auto& f=s.selection_flow;out.calls=f.calls;out.newPairs=f.new_pairs;out.proposalRetries=f.proposal_retries;out.cursorRepeats=f.cursor_repeats;
    out.lockMisses=f.lock_misses;out.lockRepeats=f.lock_repeats;out.emptyNoHistory=f.empty_no_history;out.emptyExpired=f.empty_expired_history;out.emptyOther=f.empty_other;
    out.resetRequests=s.history_reset_requests;out.resets=s.history_resets;return out;
}
struct OwnBridgeRecord {NativeGenerationBridge* bridge;OwnSelectionWitness& witness;~OwnBridgeRecord(){if(witness.attempt)bridge->RecordOwnWitness(witness);}};
std::atomic<uint64_t> nextHistoryEpoch{};
uint64_t NewHistoryEpoch() {return nextHistoryEpoch.fetch_add(1,std::memory_order_relaxed)+1;}
bool SameDevice(ID3D12DeviceChild* child,ID3D12Device* device) {
    Microsoft::WRL::ComPtr<ID3D12Device> actual;
    return child && device && SUCCEEDED(child->GetDevice(IID_PPV_ARGS(&actual))) &&
        cvr::gpu::SameNativeDevice(actual.Get(),device);
}
bool Shape(const D3D12_RESOURCE_DESC& d) {
    return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width && d.Width<=16384 &&
        d.Height && d.Height<=16384 && d.DepthOrArraySize==1 && d.MipLevels==1 && d.SampleDesc.Count==1;
}
bool ColorFormat(DXGI_FORMAT format) {
    return format==DXGI_FORMAT_R8G8B8A8_UNORM || format==DXGI_FORMAT_R8G8B8A8_TYPELESS || format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}
bool MotionFormat(DXGI_FORMAT format) {
    return format==DXGI_FORMAT_R16G16_FLOAT || format==DXGI_FORMAT_R32G32_FLOAT || format==DXGI_FORMAT_R16G16B16A16_FLOAT;
}
bool ValidView(const XrPosef& p,const XrFovf& f) {
    const float q[]={p.orientation.x,p.orientation.y,p.orientation.z,p.orientation.w};double norm=0;
    for(float v:q) {if(!std::isfinite(v))return false;norm+=double(v)*v;}
    for(float v:{p.position.x,p.position.y,p.position.z})if(!std::isfinite(v) || std::abs(v)>1000)return false;
    for(float v:{f.angleLeft,f.angleRight,f.angleUp,f.angleDown})if(!std::isfinite(v) || std::abs(v)>=1.5707963267948966f)return false;
    return std::abs(norm-1)<.002 && f.angleLeft<f.angleRight && f.angleDown<f.angleUp;
}
// Same row-major projection conversion as the existing native
// source_math::projection_fov/pack_constants, without linking its game hooks.
bool PackCamera(const CameraConstants& raw,cpvr::SourceGuides& out) {
    if(!raw.valuePresent.all() || !raw.flagPresent.all())return false;
    out.camera=raw.values;out.camera_flags=raw.flags;
    const auto measured=[](float x){return std::isfinite(x) && std::abs(x)<3.4e38f;};
    for(auto flag:out.camera_flags)if(flag>1)return false;
    if(out.camera_flags[1]!=1 || out.camera_flags[2] || out.camera_flags[4])return false;
    auto& p=out.camera;
    for(unsigned i=0;i<32;++i)if(!measured(p[i]))return false;
    for(unsigned i=48;i<84;++i)if(!measured(p[i]))return false;
    if(!(p[0]>0 && p[5]>0) || std::abs(std::abs(p[11])-1)>.0001f || std::abs(p[15])>.0001f ||
       std::abs(p[1])+std::abs(p[2])+std::abs(p[3])+std::abs(p[4])+std::abs(p[6])+std::abs(p[7])>.0001f)return false;
    const double ox=p[8]/p[11],oy=p[9]/p[11];
    const double left=std::atan((-1-ox)/p[0]),right=std::atan((1-ox)/p[0]);
    const double down=std::atan((-1-oy)/p[5]),up=std::atan((1-oy)/p[5]);
    if(!(left<0 && right>0 && down<0 && up>0))return false;
    p[100]=float(up-down);p[101]=p[5]/p[0];
    // Only documented optional fields use the existing native defaults.
    // Projection and temporal matrices are never replaced with identities.
    bool lens=true;for(unsigned i=32;i<48;++i)lens&=measured(p[i]);
    if(!lens)for(unsigned i=32;i<48;++i)p[i]=(i-32)%5==0?1.f:0.f;
    for(unsigned i=84;i<86;++i)if(!measured(p[i]))p[i]=0;
    if(!measured(p[102]))p[102]=0;
    for(float value:p)if(!measured(value))return false;
    if(!(p[98]>0 && p[99]>p[98]))return false;
    out.camera_valid=true;out.reset=out.camera_flags[3]!=0;return true;
}
struct GuideOwnership {
    // MotionRecorder retains previous guides beyond the corresponding color
    // snapshot. That history must not pin the author's three-slot color pool.
    std::shared_ptr<const Inputs> guides;
};
bool Ready(const cpvr::GpuImage& image,ID3D12Device* device) {
    if(!image.texture || !image.ready_fence || !image.ready_value || image.ready_value==UINT64_MAX ||
       !SameDevice(image.texture.Get(),device) || !SameDevice(image.ready_fence.Get(),device))return false;
    const auto completed=image.ready_fence->GetCompletedValue();
    return completed!=UINT64_MAX && completed>=image.ready_value;
}
bool Retired(const NativeSelectionLease& lease) {
    if(!lease.submissionReported)return false;
    if(!lease.copySubmitted)return true;
    if(!lease.signalSucceeded || !lease.consumerFence || !lease.consumerValue)return false;
    const auto done=lease.consumerFence->GetCompletedValue();
    return done!=UINT64_MAX && done>=lease.consumerValue;
}
void Barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};list->ResourceBarrier(1,&b);
}
}

NativeInputResult AdaptNativeInputs(ID3D12Device* device,ID3D12CommandQueue* queue,const Frame& frame,
    const std::array<cpvr::GenerationInput,2>& previous,uint64_t generationEpoch) {
    NativeInputResult out;
    if(!device || !queue || !frame.identity.serial || !frame.identity.origin || !frame.identity.stereo ||
       !frame.identity.inputs || !frame.lease || !frame.nativeStamp.resourceEpoch)return out;
    if(!SameDevice(queue,device) || queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT) {
        out.code=NativeBridgeCode::WrongQueue;return out;
    }
    if(frame.nativeStamp.transfer!=CaptureTransfer::SrgbEncoded || frame.colorCompletion.colorState!=CaptureColorReadState) {
        out.code=NativeBridgeCode::UnsupportedResource;return out;
    }
    if(frame.nativeStamp.eyes[0].clockGeneration!=frame.nativeStamp.eyes[1].clockGeneration ||
       frame.nativeStamp.renderPublicationTimeNs<=0 || !frame.nativeStamp.publicationClockGeneration ||
       frame.nativeStamp.publicationClockGeneration!=frame.nativeStamp.eyes[0].clockGeneration) {
        out.code=NativeBridgeCode::MissingCameraTime;return out;
    }
    if(!frame.colorCompletion.HasScheduledCopySource() || !SameDevice(frame.colorCompletion.fence.Get(),device)) {
        out.code=NativeBridgeCode::MissingProducerFence;return out;
    }
    for(unsigned eye=0;eye<2;++eye) {
        const auto& stamp=frame.nativeStamp.eyes[eye];const auto& input=frame.inputs[eye];
        if(!input || !input->submitted || !input->evaluated || !input->hasDepth || !input->hasMotion ||
           stamp.sourceSerial!=frame.identity.serial || stamp.poseId!=frame.identity.pose[eye] ||
           stamp.poseId!=input->poseId || stamp.poseWriteId!=input->poseWriteId || !stamp.poseWriteId ||
           stamp.spaceEpoch!=frame.identity.origin || input->origin!=stamp.spaceEpoch ||
           !stamp.nativeFrameKnown || stamp.nativeFrame!=input->frameId || stamp.nativeFrame!=frame.identity.nativeFrame ||
           stamp.engineView!=input->view || stamp.engineView!=frame.engineView[eye] || input->view<1 || input->view>2 ||
           frame.lease->resources[eye]!=frame.color[eye] || !ValidView(frame.pose[eye],frame.fov[eye]))return out;
        if(input->queue!=queue) {out.code=NativeBridgeCode::WrongQueue;return out;}
        if(!stamp.clockGeneration || stamp.clockGeneration!=input->clockGeneration || stamp.cameraApplicationQpc<=0 ||
           stamp.cameraApplicationQpc!=input->cameraApplicationQpc || stamp.contentTimeNs<=0 || stamp.contentTimeNs!=input->contentTimeNs) {
            out.code=NativeBridgeCode::MissingCameraTime;return out;
        }
        if(stamp.posePredictionNs<=0 || stamp.posePredictionNs!=input->posePredictionNs) {
            out.code=NativeBridgeCode::MissingPoseTime;return out;
        }
        if(!input->readyFence || !input->fenceValue || input->fenceValue==UINT64_MAX || !SameDevice(input->readyFence.Get(),device)) {
            out.code=NativeBridgeCode::MissingProducerFence;return out;
        }
        if(!SameDevice(frame.color[eye],device) || !SameDevice(input->depth.Get(),device) || !SameDevice(input->motion.Get(),device)) {
            out.code=NativeBridgeCode::UnsupportedResource;return out;
        }
        const auto color=frame.color[eye]->GetDesc(),depth=input->depth->GetDesc(),motion=input->motion->GetDesc();
        if(!Shape(color) || !Shape(depth) || !Shape(motion) || !ColorFormat(color.Format) ||
           depth.Format!=DXGI_FORMAT_R32_FLOAT || !MotionFormat(motion.Format) ||
           depth.Width!=motion.Width || depth.Height!=motion.Height ||
           depth.Width!=input->width || depth.Height!=input->height || motion.Width!=input->motionWidth || motion.Height!=input->motionHeight) {
            out.code=NativeBridgeCode::UnsupportedResource;return out;
        }
        auto guides=std::make_shared<cpvr::SourceGuides>();
        if(!PackCamera(input->camera.constants,*guides)) {out.code=NativeBridgeCode::IncompleteCamera;return out;}
        auto ownership=std::make_shared<GuideOwnership>();ownership->guides=input;
        guides->depth={input->depth,input->readyFence,input->fenceValue,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,ownership};
        guides->motion={input->motion,input->readyFence,input->fenceValue,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,ownership};
        auto native=std::make_shared<cpvr::EyeFrame>();native->eye=cpvr::Eye(eye);
        native->source_id=frame.identity.serial;native->stereo_pair_id=frame.identity.serial;
        native->epoch=generationEpoch?generationEpoch:frame.nativeStamp.resourceEpoch;
        // Both pixels belong to this measured paired render publication. The
        // original per-eye camera application/pose stamps above remain checked
        // and unchanged; a stationary MAIN camera can reuse those stamps while
        // many new images are rendered. Its old pose time is not image age.
        native->space_epoch=frame.identity.origin;native->content_time_ns=frame.nativeStamp.renderPublicationTimeNs;
        const auto& pose=frame.pose[eye];const auto& fov=frame.fov[eye];
        native->render_pose={{pose.orientation.x,pose.orientation.y,pose.orientation.z,pose.orientation.w},
            {pose.position.x,pose.position.y,pose.position.z}};
        native->fov={fov.angleLeft,fov.angleRight,fov.angleUp,fov.angleDown};
        native->color={frame.color[eye],frame.colorCompletion.fence,frame.colorCompletion.fenceValue,
            frame.colorCompletion.colorState,frame.lease};native->guides=guides;
        native->color.source_format=color.Format;native->color.transfer=cpvr::ColorTransfer::SrgbEncoded;
        auto& converted=out.inputs[eye];converted.frame=native;converted.native_frame_token=input->frameId;
        converted.motion_convention=cpvr::MotionConvention::NativeStreamlineBackwardUvScaled;
        const auto& old=previous[eye];
        if(old.frame && old.frame->eye==native->eye && old.frame->epoch==native->epoch && old.frame->space_epoch==native->space_epoch &&
           old.frame->source_id<native->source_id && old.frame->content_time_ns<native->content_time_ns &&
           uint32_t(input->frameId-uint32_t(old.native_frame_token))==1 && !guides->reset) {
            converted.guides_previous_source_id=old.frame->source_id;converted.guides_previous_time_ns=old.frame->content_time_ns;
            converted.previous_native_frame_token=old.native_frame_token;
        }
    }
    if(frame.inputs[0]->view==frame.inputs[1]->view) {out.inputs={};return out;}
    out.code=NativeBridgeCode::Ready;return out;
}

NativeAlternatingInputResult AdaptAlternatingMainInput(ID3D12Device* device,ID3D12CommandQueue* queue,const Frame& frame,
    cpvr::Eye physicalEye,const NativeAlternatingInputResult& previousSameEye,
    const NativeAlternatingInputResult& immediatePrevious,uint64_t generationEpoch) {
    NativeAlternatingInputResult out;
    const unsigned eye=static_cast<unsigned>(physicalEye);
    if(!device||!queue||eye>1||!frame.identity.serial||!frame.identity.origin||!frame.lease||!frame.nativeStamp.resourceEpoch)return out;
    if(!SameDevice(queue,device)||queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT){out.code=NativeBridgeCode::WrongQueue;return out;}
    if(frame.nativeStamp.transfer!=CaptureTransfer::SrgbEncoded||frame.colorCompletion.colorState!=CaptureColorReadState){
        out.code=NativeBridgeCode::UnsupportedResource;return out;
    }
    if(!frame.colorCompletion.HasScheduledCopySource()||!SameDevice(frame.colorCompletion.fence.Get(),device)||
       frame.colorCompletion.fence->GetCompletedValue()==UINT64_MAX){out.code=NativeBridgeCode::MissingProducerFence;return out;}
    const auto& stamp=frame.nativeStamp.eyes[eye];const auto& input=frame.inputs[eye];const auto& receipt=stamp.mainEyeSource;
    if(!receipt||static_cast<unsigned>(receipt.physicalEye)!=eye||receipt.epochs.hmdWorld!=frame.identity.origin||
       !input||!input->submitted||!input->evaluated||!input->hasDepth||!input->hasMotion||
       stamp.sourceSerial!=frame.identity.serial||!stamp.poseId||stamp.poseId!=frame.identity.pose[eye]||
       stamp.poseId!=input->poseId||stamp.poseWriteId!=input->poseWriteId||!stamp.poseWriteId||
       stamp.spaceEpoch!=frame.identity.origin||input->origin!=stamp.spaceEpoch||
       !stamp.nativeFrameKnown||stamp.nativeFrame!=input->frameId||stamp.nativeFrame!=frame.identity.nativeFrame||
       stamp.engineView!=1||input->view!=1||frame.engineView[eye]!=1||
       frame.lease->resources[eye]!=frame.color[eye]||!ValidView(frame.pose[eye],frame.fov[eye]))return out;
    if(input->queue!=queue){out.code=NativeBridgeCode::WrongQueue;return out;}
    if(!stamp.clockGeneration||stamp.clockGeneration!=input->clockGeneration||
       stamp.cameraApplicationQpc<=0||stamp.cameraApplicationQpc!=input->cameraApplicationQpc||
       stamp.contentTimeNs<=0||stamp.contentTimeNs!=input->contentTimeNs){out.code=NativeBridgeCode::MissingCameraTime;return out;}
    if(stamp.posePredictionNs<=0||stamp.posePredictionNs!=input->posePredictionNs||
       !receipt.MatchesWrittenPose(stamp.spaceEpoch,stamp.posePredictionNs)){out.code=NativeBridgeCode::MissingPoseTime;return out;}
    if(!input->readyFence||!input->fenceValue||input->fenceValue==UINT64_MAX||!SameDevice(input->readyFence.Get(),device)||
       input->readyFence->GetCompletedValue()==UINT64_MAX){out.code=NativeBridgeCode::MissingProducerFence;return out;}
    if(!SameDevice(frame.color[eye],device)||!SameDevice(input->depth.Get(),device)||!SameDevice(input->motion.Get(),device)){
        out.code=NativeBridgeCode::UnsupportedResource;return out;
    }
    const auto color=frame.color[eye]->GetDesc(),depth=input->depth->GetDesc(),motion=input->motion->GetDesc();
    if(!Shape(color)||!Shape(depth)||!Shape(motion)||color.SampleDesc.Quality||depth.SampleDesc.Quality||motion.SampleDesc.Quality||
       !ColorFormat(color.Format)||depth.Format!=DXGI_FORMAT_R32_FLOAT||!MotionFormat(motion.Format)||
       depth.Width!=motion.Width||depth.Height!=motion.Height||depth.Width!=input->width||depth.Height!=input->height||
       motion.Width!=input->motionWidth||motion.Height!=input->motionHeight){out.code=NativeBridgeCode::UnsupportedResource;return out;}
    auto guides=std::make_shared<cpvr::SourceGuides>();
    if(!PackCamera(input->camera.constants,*guides)){out.code=NativeBridgeCode::IncompleteCamera;return out;}
    auto ownership=std::make_shared<GuideOwnership>();ownership->guides=input;
    guides->depth={input->depth,input->readyFence,input->fenceValue,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,ownership};
    guides->motion={input->motion,input->readyFence,input->fenceValue,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,ownership};
    auto native=std::make_shared<cpvr::EyeFrame>();native->eye=physicalEye;
    // AER's first source is zero. The existing worker reserves zero for an
    // unknown predecessor; only its source-id namespace is translated by +1.
    native->source_id=uint64_t(receipt.sourceId)+1;native->stereo_pair_id=0;
    native->epoch=generationEpoch?generationEpoch:frame.nativeStamp.resourceEpoch;
    native->space_epoch=frame.identity.origin;native->content_time_ns=stamp.contentTimeNs;
    const auto& pose=frame.pose[eye];const auto& fov=frame.fov[eye];
    native->render_pose={{pose.orientation.x,pose.orientation.y,pose.orientation.z,pose.orientation.w},
        {pose.position.x,pose.position.y,pose.position.z}};
    native->fov={fov.angleLeft,fov.angleRight,fov.angleUp,fov.angleDown};native->guides=guides;
    native->color={frame.color[eye],frame.colorCompletion.fence,frame.colorCompletion.fenceValue,frame.colorCompletion.colorState,frame.lease};
    native->color.source_format=color.Format;native->color.transfer=cpvr::ColorTransfer::SrgbEncoded;
    out.input.frame=native;out.input.native_frame_token=input->frameId;
    out.input.motion_convention=cpvr::MotionConvention::NativeStreamlineBackwardUvScaled;
    out.sourceStamp=stamp;out.captureResourceEpoch=frame.nativeStamp.resourceEpoch;out.colorDescriptor=color;out.producerQueue=queue;
    out.code=NativeBridgeCode::Ready;

    // History is a separate admission decision: a bad/unknown chain never
    // hides a valid real input, nor invents a two-frame motion span.
    const auto scheduled=[&](const cpvr::GpuImage& image){
        return image.texture&&image.lifetime&&image.ready_fence&&image.ready_value&&image.ready_value!=UINT64_MAX&&
            SameDevice(image.texture.Get(),device)&&SameDevice(image.ready_fence.Get(),device)&&
            image.ready_fence->GetCompletedValue()!=UINT64_MAX;
    };
    const auto historical=[&](const NativeAlternatingInputResult& old){
        const auto& f=old.input.frame;const auto& s=old.sourceStamp;const auto& r=s.mainEyeSource;
        if(!old||!f||f->generated||!f->guides||!f->guides->camera_valid||!r||
           old.producerQueue.Get()!=queue||old.captureResourceEpoch!=out.captureResourceEpoch||
           r.epochs!=receipt.epochs||r.requestRevision!=receipt.requestRevision||r.latchRevision!=receipt.latchRevision||
           r.clockGeneration!=receipt.clockGeneration||s.clockGeneration!=stamp.clockGeneration||
           !s.nativeFrameKnown||s.engineView!=1||s.nativeFrame!=old.input.native_frame_token||
           s.spaceEpoch!=stamp.spaceEpoch||!r.MatchesWrittenPose(s.spaceEpoch,s.posePredictionNs)||
           !s.sourceSerial||s.sourceSerial>=stamp.sourceSerial||s.cameraApplicationQpc<=0||s.cameraApplicationQpc>=stamp.cameraApplicationQpc||
           s.contentTimeNs<=0||s.contentTimeNs>=stamp.contentTimeNs||s.contentTimeNs!=f->content_time_ns||
           f->eye!=cpvr::Eye(r.physicalEye)||f->source_id!=uint64_t(r.sourceId)+1||f->stereo_pair_id||
           f->epoch!=native->epoch||f->space_epoch!=native->space_epoch||
           old.input.motion_convention!=out.input.motion_convention||
           !scheduled(f->guides->depth)||!scheduled(f->guides->motion))return false;
        if(old.guideHistoryOnly){
            if(f->color.texture||f->color.lifetime||f->color.ready_fence||f->color.ready_value)return false;
        }else if(!scheduled(f->color)||f->color.transfer!=cpvr::ColorTransfer::SrgbEncoded||
                 f->color.resting_state!=CaptureColorReadState)return false;
        const auto c=old.guideHistoryOnly?old.colorDescriptor:f->color.texture->GetDesc();
        const auto d=f->guides->depth.texture->GetDesc(),m=f->guides->motion.texture->GetDesc();
        return Shape(c)&&Shape(d)&&Shape(m)&&!c.SampleDesc.Quality&&!d.SampleDesc.Quality&&!m.SampleDesc.Quality&&
            ColorFormat(c.Format)&&d.Format==DXGI_FORMAT_R32_FLOAT&&MotionFormat(m.Format)&&
            c.Width==color.Width&&c.Height==color.Height&&d.Width==depth.Width&&d.Height==depth.Height&&
            m.Width==motion.Width&&m.Height==motion.Height;
    };
    const auto adjacent=[](const NativeAlternatingInputResult& older,const NativeAlternatingInputResult& newer){
        const auto& a=older.sourceStamp;const auto& b=newer.sourceStamp;
        return b.mainEyeSource.sourceId-a.mainEyeSource.sourceId==1&&
            b.mainEyeSource.sceneOrdinal>a.mainEyeSource.sceneOrdinal&&b.mainEyeSource.sceneOrdinal-a.mainEyeSource.sceneOrdinal==1&&
            uint32_t(b.nativeFrame-a.nativeFrame)==1&&a.contentTimeNs<b.contentTimeNs;
    };
    const auto& mid=immediatePrevious;
    if(!guides->reset&&historical(mid)&&mid.input.frame->eye!=physicalEye&&adjacent(mid,out)&&
       !mid.input.frame->guides->reset&&!mid.input.frame->guides->camera_flags[3]&&mid.input.native_frame_token){
        out.input.guides_previous_source_id=mid.input.frame->source_id;
        out.input.guides_previous_time_ns=mid.input.frame->content_time_ns;
        out.input.previous_native_frame_token=mid.input.native_frame_token;
        out.history=NativeAlternatingHistory::AdjacentNative;
        const auto& previous=previousSameEye;
        if(historical(previous)&&previous.input.frame->eye==physicalEye&&adjacent(previous,mid)&&
           uint32_t(stamp.nativeFrame-previous.sourceStamp.nativeFrame)==2&&
           mid.input.guides_previous_source_id==previous.input.frame->source_id&&
           mid.input.guides_previous_time_ns==previous.input.frame->content_time_ns&&
           mid.input.previous_native_frame_token==previous.input.native_frame_token){
            auto middleGuides=std::make_shared<cpvr::EyeFrame>(*mid.input.frame);
            middleGuides->color={}; // Never retain a middle capture's color ReadLease.
            out.input.immediate_previous={middleGuides,mid.input.native_frame_token,mid.input.previous_native_frame_token,
                mid.input.guides_previous_source_id,mid.input.guides_previous_time_ns,mid.input.motion_convention};
            const auto sample=[](const cpvr::GenerationInput& value){return cpvr::MotionGuideSample{value.frame->guides,value.frame->source_id,
                value.native_frame_token,value.previous_native_frame_token,value.guides_previous_source_id,value.frame->content_time_ns,
                value.guides_previous_time_ns,value.motion_convention};};
            cpvr::MotionRecordRequest request;request.previous=sample(previous.input);request.current=sample(out.input);
            request.middle=cpvr::MotionGuideSample{middleGuides->guides,middleGuides->source_id,mid.input.native_frame_token,
                mid.input.previous_native_frame_token,mid.input.guides_previous_source_id,middleGuides->content_time_ns,
                mid.input.guides_previous_time_ns,mid.input.motion_convention};
            if(cpvr::validate_motion_request(request).valid)out.history=NativeAlternatingHistory::SameEyeDelta2;
            else out.input.immediate_previous={};
        }
    }
    return out;
}

NativeAlternatingInputResult AlternatingMainGuideHistory(const NativeAlternatingInputResult& source) {
    if(!source||!source.input.frame||!source.input.frame->guides)return {};
    auto out=source;auto frame=std::make_shared<cpvr::EyeFrame>(*source.input.frame);
    frame->color={};out.input.frame=std::move(frame);out.input.immediate_previous={};out.guideHistoryOnly=true;
    return out;
}

struct NativeGenerationBridge::Impl {
    struct PairedMetadata {
        uint64_t serial{};
        NativeFrameStamp stamp;
        std::array<std::optional<FrameProjectionMetadata>,2> projection;
        std::array<uint32_t,2> view{};
        std::array<uint64_t,2> pose{};
        std::array<std::array<uint32_t,2>,2> extent{};
    };
    // Value metadata only: no capture/color/depth/MV leases. Selected products
    // keep their own original projection rather than borrowing the newest one.
    std::deque<PairedMetadata> pairedMetadata;
    enum class SourceKind {Unknown,Paired,Alternating};
    cpvr::GenerationWorker* worker;
    std::array<cpvr::GenerationInput,2> previous;
    std::array<NativeAlternatingInputResult,2> alternatingPrevious;
    NativeAlternatingInputResult alternatingImmediate;
    MainEyeSourceReceipt alternatingOwner;
    int64_t admittedAlternating=-1,seenAlternating=-1;
    SourceKind sourceKind=SourceKind::Unknown;
    std::array<std::shared_ptr<NativeSelectionLease>,8> consumers;
    NativeBridgeStatus status;
    uint64_t admittedSerial{};
    uint64_t historyEpoch=NewHistoryEpoch(),resourceEpoch{},spaceEpoch{},clockGeneration{},seenSerial{};
    Microsoft::WRL::ComPtr<IUnknown> deviceIdentity;
    bool dormant{},sourceSignature{},cameraReset{};
    std::optional<cpvr::GenerationConfig> configuration;cpvr::Log log;bool startAttempted{};
    OwnWitnessRing witnesses;
    explicit Impl(cpvr::GenerationWorker& value):worker(&value) {}
};
NativeGenerationBridge::NativeGenerationBridge(cpvr::GenerationWorker& worker):impl(std::make_unique<Impl>(worker)) {}
NativeGenerationBridge::NativeGenerationBridge(cpvr::GenerationWorker& worker,cpvr::GenerationConfig config,cpvr::Log log)
    :NativeGenerationBridge(worker) {impl->configuration=std::move(config);impl->log=std::move(log);}
NativeGenerationBridge::~NativeGenerationBridge() {
    Collect();
    // Unknown GPU use must not release pooled products during teardown. Only
    // this bounded set of image/fence leases is retained, never another worker.
    for(const auto& lease:impl->consumers)if(lease && (lease->commandsRecorded || lease->copySubmitted) && !Retired(*lease)) {
        (void)new decltype(impl->consumers)(std::move(impl->consumers));break;
    }
}
void NativeGenerationBridge::Collect() {
    for(auto& lease:impl->consumers)if(lease && (Retired(*lease) ||
        (lease.use_count()==1 && !lease->commandsRecorded && !lease->submissionReported)))lease.reset();
    impl->status.retainedConsumers=unsigned(std::count_if(impl->consumers.begin(),impl->consumers.end(),[](const auto& item){return bool(item);}));
}
void NativeGenerationBridge::Reset() {
    if(!impl->dormant) {
        impl->dormant=true;impl->sourceSignature=false;impl->cameraReset=false;
        impl->previous={};impl->admittedSerial=0;impl->historyEpoch=NewHistoryEpoch();
        impl->pairedMetadata.clear();
        impl->alternatingPrevious={};impl->alternatingImmediate={};impl->alternatingOwner={};
        impl->admittedAlternating=impl->seenAlternating=-1;
        impl->resourceEpoch=impl->spaceEpoch=impl->clockGeneration=impl->seenSerial=0;
        impl->worker->request_history_reset();
    }
    Collect();
}
NativeBridgeStatus NativeGenerationBridge::Status() const {return impl->status;}
void NativeGenerationBridge::RecordOwnWitness(OwnSelectionWitness& w)noexcept{
    if(!w.attempt)return;
    try{impl->witnesses.Store(w);if(!impl->log||!impl->witnesses.LogDue(w,GetTickCount64()))return;
        char text[4096]{};
        std::snprintf(text,sizeof(text),"[OWN-ATTEMPT] id=%llu target=%lld period=%lld pair=%llu candidate=%u history=%llu/%llu space=%llu/%llu resource=%llu clock=%llu source=%llu admitted=%llu fallback=%llu bridge=%u bridgeEye=%u path=%u downstream=%u downstreamEye=%u output=%u mask=%u new=%u realOnly=%u calibration=%u samePair=%llu first=%lld submissionReported=%u copy=%u signal=%u validatedKnown=%u validSignal=%u accepted=%u display=%lld ackCall=%u ackKnown=%u ackReturn=%u ring=%zu evicted=%llu reasonCount=%llu",
            w.attempt,w.target,w.period,w.pair,unsigned(w.candidateKnown),w.history,w.candidateHistory,w.space,w.candidateSpace,w.resource,w.clockGeneration,w.sourceSerial,w.admittedSerial,w.fallbackSerial,unsigned(w.bridge),w.bridgeEye,unsigned(w.presenter),unsigned(w.downstream),w.downstreamEye,unsigned(w.output),w.generatedMask,unsigned(w.newPresentation),unsigned(w.realOnly),unsigned(w.baselineCalibration),w.samePairRun,w.samePairFirstTarget,unsigned(w.submissionReported),unsigned(w.copySubmitted),unsigned(w.signalReported),unsigned(w.signalValidatedKnown),unsigned(w.signalSucceeded),unsigned(w.accepted),w.displayTime,unsigned(w.ackCall),unsigned(w.ackReturnKnown),unsigned(w.ackReturn),impl->witnesses.Count(),impl->witnesses.Evicted(),impl->witnesses.ReasonCount(w.bridge));impl->log(text);
        for(unsigned eye=0;eye<2;++eye){const auto& e=w.eyes[eye];const auto& g=e.geometry;const auto& ig=w.initialGeometry.eyes[eye];
            std::snprintf(text,sizeof(text),"[OWN-ATTEMPT-EYE] id=%llu eye=%u known=%u generated=%u source=%llu content=%lld originKnown=%u previous=%llu current=%llu nativePair=%llu/%llu time=%lld/%lld generation=%u/%u nativeFrame=%u engineView=%u extent=%ux%u/%ux%u/%ux%u examined=%u/%u resolved=%u/%u compared=%u cropA=%.9g,%.9g,%.9g,%.9g cropB=%.9g,%.9g,%.9g,%.9g cropMask=%u fovA=%.9g,%.9g,%.9g,%.9g fovB=%.9g,%.9g,%.9g,%.9g delta=%.9g,%.9g,%.9g,%.9g max=%.9g threshold=.00002 displayExamined=%u displayResolved=%u displayInputMask=%u target=%ux%u displayCrop=%.9g,%.9g,%.9g,%.9g initialKnown=%u initialShape=%ux%u initialProjection=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g initialExpectedFov=%.9g,%.9g,%.9g,%.9g initialExpectedXY=%.9g,%.9g initialScaleXY=%.9g,%.9g initialCenterXY=%.9g,%.9g initialCrop=%.9g,%.9g,%.9g,%.9g initialTangents=%.9g,%.9g,%.9g,%.9g initialComputedScale=%.9g",
                w.attempt,eye,unsigned(e.known),unsigned(e.generated),e.source,e.contentTime,unsigned(e.originKnown),e.previousSource,e.currentSource,e.previousNativePair,e.currentNativePair,e.previousTime,e.currentTime,e.generationIndex,e.generationCount,e.nativeFrame,e.engineView,g.previousWidth,g.previousHeight,g.currentWidth,g.currentHeight,g.outputWidth,g.outputHeight,unsigned(g.previousExamined),unsigned(g.currentExamined),unsigned(g.previousResolved),unsigned(g.currentResolved),unsigned(g.comparisonKnown),g.previousCrop[0],g.previousCrop[1],g.previousCrop[2],g.previousCrop[3],g.currentCrop[0],g.currentCrop[1],g.currentCrop[2],g.currentCrop[3],g.cropMismatch,g.previousFov[0],g.previousFov[1],g.previousFov[2],g.previousFov[3],g.currentFov[0],g.currentFov[1],g.currentFov[2],g.currentFov[3],g.delta[0],g.delta[1],g.delta[2],g.delta[3],g.maximumDelta,unsigned(e.displayExamined),unsigned(e.displayResolved),e.displayInputMask,e.targetWidth,e.targetHeight,e.displayCrop[0],e.displayCrop[1],e.displayCrop[2],e.displayCrop[3],ig.knownBits,ig.width,ig.height,ig.projection[0],ig.projection[1],ig.projection[2],ig.projection[3],ig.projection[4],ig.projection[5],ig.expectedFov[0],ig.expectedFov[1],ig.expectedFov[2],ig.expectedFov[3],ig.expectedX,ig.expectedY,ig.scaleX,ig.scaleY,ig.centerX,ig.centerY,ig.crop[0],ig.crop[1],ig.crop[2],ig.crop[3],ig.tangents[0],ig.tangents[1],ig.tangents[2],ig.tangents[3],ig.computedScale);impl->log(text);
        }
        const auto& a=w.selectedCounters;const auto& b=w.submittedCounters;const auto& sd=w.workerDecision;
        std::snprintf(text,sizeof(text),"[OWN-ATTEMPT-ADMISSION] id=%llu examined=%u code=%u submittedMask=%u replay=%u submissionKnown=%u downstreamCount=%llu outputCount=%llu initialAdmission=%u initialLease=%u geometryExamined=%u geometryResolved=%u initialReason=%u initialEye=%u initialKnown=%u initialComponent=%u",
            w.attempt,unsigned(w.inputExamined),w.inputCode,w.inputSubmittedMask,unsigned(w.replay),unsigned(w.submissionKnown),impl->witnesses.DownstreamCount(w.downstream),impl->witnesses.OutputCount(w.output),w.initialPairedAdmission,unsigned(w.initialLease),unsigned(w.initialGeometryExamined),unsigned(w.initialGeometryResolved),unsigned(w.initialGeometry.reason),w.initialGeometry.eye,w.initialGeometry.knownBits,w.initialGeometry.component);impl->log(text);
        std::snprintf(text,sizeof(text),"[OWN-ATTEMPT-FLOW] id=%llu coherent=%u/%u maxAge=%lld calls=%llu/%llu new=%llu/%llu retries=%llu/%llu cursor=%llu/%llu lockMiss=%llu lockRepeat=%llu empty=%llu/%llu/%llu input=%llu/%llu batches=%llu/%llu products=%llu/%llu committed=%llu/%llu generatedCommitted=%llu/%llu resetReq=%llu/%llu reset=%llu/%llu selectorFresh=%u selectorKnown=%u selectorStep=%c%c selectorInputsHandled=%u selectorAvailable=%u selectorExpected=%u/%u selectorCompatible=%u selectorFuture=%u selectorExpired=%u selectorSkew=%u selectorRegressed=%u selectorEarly=%u selectorMinAge=%lld selectorMinSkew=%lld selectorDesired=%lld selectorPeriod=%lld selectorNewestR=%lld/%lld selectorNewestG=%lld/%lld",
            w.attempt,unsigned(a.coherent),unsigned(b.coherent),a.maximumAge,a.calls,b.calls,a.newPairs,b.newPairs,a.proposalRetries,b.proposalRetries,a.cursorRepeats,b.cursorRepeats,a.lockMisses,a.lockRepeats,a.emptyNoHistory,a.emptyExpired,a.emptyOther,a.acceptedInputs,b.acceptedInputs,a.completedBatches,b.completedBatches,a.generatedOutputs,b.generatedOutputs,a.selectedPairs,b.selectedPairs,a.generatedSelectedPairs,b.generatedSelectedPairs,a.resetRequests,b.resetRequests,a.resets,b.resets,unsigned(sd.freshNoCandidate),unsigned(sd.requestedKnown),sd.requestedStep[0],sd.requestedStep[1],unsigned(sd.inputsHandled),unsigned(sd.patternAvailable),sd.expectedProducts[0],sd.expectedProducts[1],sd.compatible,sd.future,sd.expired,sd.skew,sd.regressed,sd.earlyForSlot,sd.minimumAge,sd.minimumSkew,sd.desiredContent,sd.period,sd.newestReal[0],sd.newestReal[1],sd.newestGenerated[0],sd.newestGenerated[1]);impl->log(text);
    }catch(...){} // Diagnostic failure must never alter selection/retirement.
}
void NativeGenerationBridge::ObserveOwnFallback(const Selection& result,bool accepted,bool copy,bool signal,int64_t display)noexcept{
    if(!result.ownWitness.attempt)return;try{auto w=result.ownWitness;OwnWitnessSubmission(w,accepted,copy,signal,display,false,false,false);
        w.submittedCounters=WitnessCounters(impl->worker->status(),w.target);w.submittedCounters.coherent&=w.history==impl->historyEpoch;RecordOwnWitness(w);}catch(...){}
}
void NativeGenerationBridge::Maintain(PresentationTime time) {
    Collect();
    if(time.target<=0||time.period<=0)return;
    // Service bounded expiry even without a coherent new source. This must
    // not choose/cache a proposal ahead of the normal source-admission path.
    (void)impl->worker->retire_expired(time.target);
}
uint64_t NativeGenerationBridge::HistoryEpoch() const {return impl->historyEpoch;}
bool NativeGenerationBridge::Select(ID3D12Device* device,ID3D12CommandQueue* queue,const Frame& source,PresentationTime time,Selection& output) {
    auto& witness=output.ownWitness;
    if(OwnSelectionWitnessEnabled()){witness={};witness.attempt=NextOwnSelectionAttempt();witness.target=time.target;witness.period=time.period;witness.sourceSerial=source.identity.serial;witness.fallbackSerial=source.identity.serial;witness.resource=source.nativeStamp.resourceEpoch;witness.clockGeneration=source.nativeStamp.publicationClockGeneration;witness.space=source.identity.origin;witness.history=impl->historyEpoch;}
    OwnBridgeRecord witnessRecord{this,witness};
    const auto veto=[&](OwnBridgeReason reason,unsigned eye=UINT32_MAX){if(witness.attempt){witness.bridge=reason;witness.bridgeEye=eye;witness.admittedSerial=impl->admittedSerial;witness.history=impl->historyEpoch;}return false;};
    if(impl->sourceKind!=Impl::SourceKind::Unknown&&impl->sourceKind!=Impl::SourceKind::Paired){
        if(impl->dormant&&!impl->worker->status().history_reset_pending)impl->dormant=false;
        Reset();
    }
    impl->sourceKind=Impl::SourceKind::Paired;
    Collect();if(time.target<=0 || time.period<=0)return veto(OwnBridgeReason::InvalidTime);
    Microsoft::WRL::ComPtr<IUnknown> identity;
    if(!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&identity))))return veto(OwnBridgeReason::MissingDevice);
    if(impl->deviceIdentity && impl->deviceIdentity.Get()!=identity.Get()) {
        Reset();impl->status.code=NativeBridgeCode::DeviceChanged;return veto(OwnBridgeReason::DeviceChanged);
    }
    impl->deviceIdentity=identity;
    auto backend=impl->worker->status();
    static const bool historyDiagnostic=[] {char flag[8]{};return GetEnvironmentVariableA("CPVR_FG_HISTORY_DIAG",flag,sizeof(flag))==1&&flag[0]=='1';}();
    static uint64_t historySamples{};
    if(historyDiagnostic && impl->log && (++historySamples<=8||historySamples%180==0)) {
        const auto timingIndex=std::min<size_t>(backend.active_policy.generated_per_interval,backend.timings.size()-1);
        const auto& timing=backend.timings[timingIndex];
        char report[1600]{};
        std::snprintf(report,sizeof(report),"[OWN-NV-STATE] code=%u source=%llu cameraTime=%lld/%lld publication=%lld clock=%llu target=%lld accepted=%llu dropped=%llu batches=%llu generated=%llu selected=%llu expired=%llu/%llu reset=%llu/%llu inputTime=%lld/%lld unknownGuideSpans=%llu queuedInputs=%u timingG=%u queuedMeanNs=%lld queuedP95Ns=%lld snapshotMeanNs=%lld snapshotP95Ns=%lld backendMeanNs=%lld backendP95Ns=%lld phase=%s",
            unsigned(impl->status.code),source.identity.serial,(long long)source.nativeStamp.eyes[0].contentTimeNs,(long long)source.nativeStamp.eyes[1].contentTimeNs,
            (long long)source.nativeStamp.renderPublicationTimeNs,source.nativeStamp.publicationClockGeneration,(long long)time.target,
            backend.accepted_inputs,backend.dropped_inputs,backend.completed_batches,backend.generated_outputs,backend.generated_selected_pairs,
            backend.expired_batches,backend.expired_products,backend.history_reset_requests,backend.history_resets,
            (long long)backend.latest_input_time_ns[0],(long long)backend.latest_input_time_ns[1],
            (unsigned long long)backend.unknown_guide_spans,backend.queued_inputs,unsigned(timingIndex),
            (long long)timing.queued.window_mean_ns,(long long)timing.queued.p95_ns,
            (long long)timing.private_snapshot.window_mean_ns,(long long)timing.private_snapshot.p95_ns,
            (long long)timing.backend_batch.window_mean_ns,(long long)timing.backend_batch.p95_ns,backend.phase.c_str());
        impl->log(report);
    }
    if(impl->dormant && !backend.history_reset_pending)impl->dormant=false;
    const auto clock=source.nativeStamp.publicationClockGeneration;
    if(impl->sourceSignature && ((impl->resourceEpoch && source.nativeStamp.resourceEpoch && impl->resourceEpoch!=source.nativeStamp.resourceEpoch) ||
       (impl->spaceEpoch && source.identity.origin && impl->spaceEpoch!=source.identity.origin) ||
       (impl->clockGeneration && clock && impl->clockGeneration!=clock) ||
       (source.identity.reset && !impl->cameraReset)))Reset();
    if(source.nativeStamp.resourceEpoch)impl->resourceEpoch=source.nativeStamp.resourceEpoch;
    if(source.identity.origin)impl->spaceEpoch=source.identity.origin;
    if(clock)impl->clockGeneration=clock;
    impl->seenSerial=source.identity.serial;
    impl->sourceSignature=source.identity.serial && impl->resourceEpoch && impl->spaceEpoch && impl->clockGeneration;
    impl->cameraReset=source.identity.reset;
    auto slot=std::find(impl->consumers.begin(),impl->consumers.end(),nullptr);
    if(slot==impl->consumers.end()) {impl->status.code=NativeBridgeCode::ConsumerCapacity;return veto(OwnBridgeReason::ConsumerCapacity);}
    backend=impl->worker->status();
    if(backend.failed) {impl->status.code=NativeBridgeCode::BackendFailed;return veto(OwnBridgeReason::BackendFailed);}
    if(backend.history_reset_pending && backend.worker_running) {impl->status.code=NativeBridgeCode::HistoryResetPending;return veto(OwnBridgeReason::HistoryResetPending);}
    if(source.identity.serial!=impl->admittedSerial) {
        const auto adapted=AdaptNativeInputs(device,queue,source,impl->previous,impl->historyEpoch);impl->status.code=adapted.code;
        if(witness.attempt){witness.inputExamined=true;witness.inputCode=unsigned(adapted.code);}
        if(adapted && std::none_of(impl->pairedMetadata.begin(),impl->pairedMetadata.end(),[&](const auto& m){return m.serial==source.identity.serial;})) {
            Impl::PairedMetadata metadata;metadata.serial=source.identity.serial;metadata.stamp=source.nativeStamp;
            for(unsigned eye=0;eye<2;++eye){metadata.projection[eye]=source.projection[eye];metadata.view[eye]=source.engineView[eye];metadata.pose[eye]=source.identity.pose[eye];
                const auto desc=source.color[eye]->GetDesc();metadata.extent[eye]={uint32_t(desc.Width),desc.Height};}
            impl->pairedMetadata.push_back(std::move(metadata));
            while(impl->pairedMetadata.size()>64)impl->pairedMetadata.pop_front();
        }
        if(adapted && !backend.worker_running && impl->configuration && !impl->startAttempted) {
            impl->startAttempted=true;
            if(!impl->worker->start(device,*impl->configuration,impl->log)) {impl->status.code=NativeBridgeCode::BackendFailed;return veto(OwnBridgeReason::BackendFailed);}
            backend=impl->worker->status();
        }
        if(backend.history_reset_pending) {impl->status.code=NativeBridgeCode::HistoryResetPending;return veto(OwnBridgeReason::HistoryResetPending);}
        impl->dormant=false;
        if(adapted && backend.backend_attached && backend.worker_running) {
            bool admitted=true;
            for(unsigned eye=0;eye<2;++eye) {
                if(impl->worker->submit(adapted.inputs[eye])){
                    if(witness.attempt)witness.inputSubmittedMask|=1u<<eye;
                    // Paired adjacency only needs the previous identity/time
                    // and native token. GPU work owns the submitted input in
                    // the worker; this bookkeeping must not pin capture or
                    // guide slots after that work and its consumers retire.
                    auto history=adapted.inputs[eye];auto metadata=std::make_shared<cpvr::EyeFrame>(*history.frame);
                    metadata->color={};metadata->guides.reset();history.frame=std::move(metadata);history.immediate_previous={};
                    impl->previous[eye]=std::move(history);
                }else admitted=false;
            }
            if(admitted)impl->admittedSerial=source.identity.serial;
        } else if(adapted)impl->status.code=NativeBridgeCode::BackendPending;
    }
    if(backend.history_reset_pending) {impl->status.code=NativeBridgeCode::HistoryResetPending;return veto(OwnBridgeReason::HistoryResetPending);}
    impl->dormant=false;
    // A separate mode owner may also feed this same worker. Missing current
    // author inputs must not hide an already valid, timely selected product.
    const auto selected=impl->worker->select(time.target,time.period);
    if(witness.attempt){CaptureOwnWorkerDecision(witness.workerDecision,selected);witness.history=impl->historyEpoch;witness.admittedSerial=impl->admittedSerial;
        try{witness.selectedCounters=WitnessCounters(impl->worker->status(),time.target);}catch(...){}
        if(selected.frame){witness.candidateKnown=true;witness.pair=selected.frame->pair_id;witness.candidateHistory=selected.frame->epoch;witness.candidateSpace=selected.frame->space_epoch;witness.newPresentation=selected.new_presentation;
            for(unsigned eye=0;eye<2;++eye)if(const auto& value=selected.frame->eyes[eye]){auto& e=witness.eyes[eye];const auto& o=selected.origins[eye];e.known=true;e.generated=value->generated;e.source=value->source_id;e.contentTime=value->content_time_ns;e.originKnown=o.generated;e.previousSource=o.previous_source_id;e.currentSource=o.current_source_id;e.previousNativePair=o.previous_native_pair_id;e.currentNativePair=o.current_native_pair_id;e.previousTime=o.previous_time_ns;e.currentTime=o.current_time_ns;e.generationIndex=o.index;e.generationCount=o.count;if(value->generated)witness.generatedMask|=1u<<eye;}}
    }
    if(!selected.frame)return veto(OwnBridgeReason::NoCandidate);
    if(selected.frame->epoch!=impl->historyEpoch || (source.identity.origin && selected.frame->space_epoch!=source.identity.origin))return veto(OwnBridgeReason::EpochSpaceMismatch);
    bool generated=false;
    std::array<const Impl::PairedMetadata*,2> metadata{};
    for(unsigned eye=0;eye<2;++eye) {
        const auto& value=selected.frame->eyes[eye];
        if(!value || value->eye!=cpvr::Eye(eye) || !Ready(value->color,device))return veto(OwnBridgeReason::EyeOrColorNotReady,eye);
        const auto found=std::find_if(impl->pairedMetadata.begin(),impl->pairedMetadata.end(),[&](const auto& m){
            return m.serial==value->source_id && m.stamp.resourceEpoch==impl->resourceEpoch &&
                m.stamp.eyes[eye].spaceEpoch==selected.frame->space_epoch;});
        if(found==impl->pairedMetadata.end()){impl->status.code=NativeBridgeCode::MissingIdentity;return veto(OwnBridgeReason::CurrentMetadataMissing,eye);}
        metadata[eye]=&*found;
        const auto desc=value->color.texture->GetDesc();
        if(witness.attempt){witness.eyes[eye].nativeFrame=found->stamp.eyes[eye].nativeFrame;witness.eyes[eye].engineView=found->view[eye];witness.eyes[eye].geometry.outputWidth=uint32_t(desc.Width);witness.eyes[eye].geometry.outputHeight=desc.Height;}
        if(value->generated){
            const auto& origin=selected.origins[eye];
            const auto previous=std::find_if(impl->pairedMetadata.begin(),impl->pairedMetadata.end(),[&](const auto& m){
                return m.serial==origin.previous_source_id && m.stamp.resourceEpoch==impl->resourceEpoch &&
                    m.stamp.eyes[eye].spaceEpoch==selected.frame->space_epoch;});
            if(!origin.generated||origin.current_source_id!=value->source_id)return veto(OwnBridgeReason::GeneratedOriginMismatch,eye);
            if(previous==impl->pairedMetadata.end())return veto(OwnBridgeReason::PreviousMetadataMissing,eye);
            if(!previous->projection[eye]||!found->projection[eye])return veto(OwnBridgeReason::ProjectionMissing,eye);
            AerRealGeometry a,b;a.width=previous->extent[eye][0];a.height=previous->extent[eye][1];
            b.width=found->extent[eye][0];b.height=found->extent[eye][1];
            if(witness.attempt){auto& g=witness.eyes[eye].geometry;g.previousWidth=a.width;g.previousHeight=a.height;g.currentWidth=b.width;g.currentHeight=b.height;}
            if(a.width!=b.width||a.height!=b.height||b.width!=desc.Width||b.height!=desc.Height)return veto(OwnBridgeReason::ExtentMismatch,eye);
            const bool previousResolved=AerRealPresenter::ResolveCapturedGeometry(previous->projection[eye]->constants,previous->stamp.eyes[eye].referenceOptics,eye,a);
            if(witness.attempt){auto& g=witness.eyes[eye].geometry;g.previousExamined=true;g.previousResolved=previousResolved;if(previousResolved){g.previousCrop=a.crop;g.previousFov={a.renderedFov.angleLeft,a.renderedFov.angleRight,a.renderedFov.angleUp,a.renderedFov.angleDown};}}
            if(!previousResolved)return veto(OwnBridgeReason::PreviousGeometryUnresolved,eye);
            const bool currentResolved=AerRealPresenter::ResolveCapturedGeometry(found->projection[eye]->constants,found->stamp.eyes[eye].referenceOptics,eye,b);
            if(witness.attempt){auto& g=witness.eyes[eye].geometry;g.currentExamined=true;g.currentResolved=currentResolved;if(currentResolved){g.currentCrop=b.crop;g.currentFov={b.renderedFov.angleLeft,b.renderedFov.angleRight,b.renderedFov.angleUp,b.renderedFov.angleDown};}g.Compare();}
            if(!currentResolved)return veto(OwnBridgeReason::CurrentGeometryUnresolved,eye);
            if(a.crop!=b.crop)return veto(OwnBridgeReason::CropMismatch,eye);
            if(std::abs(a.renderedFov.angleLeft-b.renderedFov.angleLeft)>=.00002f||
               std::abs(a.renderedFov.angleRight-b.renderedFov.angleRight)>=.00002f||
               std::abs(a.renderedFov.angleUp-b.renderedFov.angleUp)>=.00002f||
               std::abs(a.renderedFov.angleDown-b.renderedFov.angleDown)>=.00002f)return veto(OwnBridgeReason::RenderedFovDelta,eye);
        }
        if(source.color[eye]) {
            const auto target=source.color[eye]->GetDesc();
            if(desc.Width!=target.Width || desc.Height!=target.Height)return veto(OwnBridgeReason::LiveSourceExtentMismatch,eye);
        }
        const auto state=value->color.resting_state;
        const bool readable=value->generated ?
            (state==D3D12_RESOURCE_STATE_COMMON || state==D3D12_RESOURCE_STATE_COPY_SOURCE) : state==CaptureColorReadState;
        if(!Shape(desc) || !ColorFormat(desc.Format) || value->color.transfer!=cpvr::ColorTransfer::SrgbEncoded || !readable)return veto(OwnBridgeReason::ResourceShapeFormatTransferState,eye);
        generated|=value->generated;
    }
    if(generated && !backend.backend_attached) {impl->status.code=NativeBridgeCode::BackendPending;return veto(OwnBridgeReason::BackendDetached);}
    auto lease=std::make_shared<NativeSelectionLease>();lease->selected=selected;lease->device=device;*slot=lease;
    if(witness.attempt)ResetSelectionPreservingOwnWitness(output);else output={};output.native=lease;output.synthetic=generated;
    output.frame.nativeStamp=metadata[0]->stamp;
    output.kind=selected.new_presentation ? (generated?Cadence::Next::Midpoint:Cadence::Next::Real) : Cadence::Next::Repeat;
    output.frame.identity.serial=std::max(selected.frame->eyes[0]->source_id,selected.frame->eyes[1]->source_id);
    output.frame.identity.origin=selected.frame->space_epoch;output.frame.identity.stereo=true;
    for(unsigned eye=0;eye<2;++eye) {
        const auto& value=*selected.frame->eyes[eye];output.frame.color[eye]=value.color.texture.Get();
        output.frame.nativeStamp.eyes[eye]=metadata[eye]->stamp.eyes[eye];output.frame.projection[eye]=metadata[eye]->projection[eye];
        output.frame.engineView[eye]=metadata[eye]->view[eye];output.frame.identity.pose[eye]=value.generated?0:metadata[eye]->pose[eye];
        const auto& p=value.render_pose;output.frame.pose[eye]={{p.orientation[0],p.orientation[1],p.orientation[2],p.orientation[3]},
            {p.position[0],p.position[1],p.position[2]}};
        const auto& f=value.fov;output.frame.fov[eye]={f.left,f.right,f.up,f.down};
    }
    impl->status.code=NativeBridgeCode::Ready;if(witness.attempt)witness.bridge=OwnBridgeReason::Ready;return true;
}
NativeAlternatingIngestResult NativeGenerationBridge::IngestAlternating(ID3D12Device* device,ID3D12CommandQueue* queue,
    const Frame& source,cpvr::Eye physicalEye) {
    using Status=NativeAlternatingIngestStatus;
    const auto result=[&](Status status,NativeBridgeCode code,NativeAlternatingHistory history=NativeAlternatingHistory::Warmup){
        impl->status.code=code;return NativeAlternatingIngestResult{status,code,history};
    };
    if(impl->sourceKind!=Impl::SourceKind::Unknown&&impl->sourceKind!=Impl::SourceKind::Alternating){
        if(impl->dormant&&!impl->worker->status().history_reset_pending)impl->dormant=false;
        Reset();
    }
    impl->sourceKind=Impl::SourceKind::Alternating;
    Collect();
    Microsoft::WRL::ComPtr<IUnknown> identity;
    if(!device||FAILED(device->QueryInterface(IID_PPV_ARGS(&identity))))return result(Status::Rejected,NativeBridgeCode::MissingIdentity);
    if(impl->deviceIdentity&&impl->deviceIdentity.Get()!=identity.Get()){
        Reset();return result(Status::Rejected,NativeBridgeCode::DeviceChanged);
    }
    impl->deviceIdentity=identity;
    auto backend=impl->worker->status();
    if(impl->dormant&&!backend.history_reset_pending)impl->dormant=false;
    const unsigned eye=static_cast<unsigned>(physicalEye);
    if(eye>1)return result(Status::Rejected,NativeBridgeCode::MissingIdentity);
    const auto& stamp=source.nativeStamp.eyes[eye];const auto& owner=stamp.mainEyeSource;
    if(!owner||static_cast<unsigned>(owner.physicalEye)!=eye||owner.epochs.hmdWorld!=source.identity.origin){
        return result(Status::Rejected,NativeBridgeCode::MissingIdentity);
    }
    const auto& priorOwner=impl->alternatingOwner;
    const bool changedOwner=priorOwner&&(priorOwner.epochs!=owner.epochs||priorOwner.requestRevision!=owner.requestRevision||
        priorOwner.latchRevision!=owner.latchRevision||priorOwner.clockGeneration!=owner.clockGeneration);
    const bool changedSignature=changedOwner||(impl->sourceSignature&&
       ((impl->resourceEpoch&&source.nativeStamp.resourceEpoch&&impl->resourceEpoch!=source.nativeStamp.resourceEpoch)||
        (impl->spaceEpoch&&source.identity.origin&&impl->spaceEpoch!=source.identity.origin)||
        (impl->clockGeneration&&stamp.clockGeneration&&impl->clockGeneration!=stamp.clockGeneration)||
        (source.identity.reset&&!impl->cameraReset)));
    // A stale domain cannot reset current history and become fresh again.
    // All these generations belong to monotonic owners in this bridge lifetime.
    if(priorOwner&&(owner.epochs.renderer<priorOwner.epochs.renderer||owner.epochs.gameWorld<priorOwner.epochs.gameWorld||
       owner.epochs.hmdWorld<priorOwner.epochs.hmdWorld||owner.requestRevision<priorOwner.requestRevision||
       owner.latchRevision<priorOwner.latchRevision||owner.clockGeneration<priorOwner.clockGeneration||
       source.nativeStamp.resourceEpoch<impl->resourceEpoch||stamp.clockGeneration<impl->clockGeneration))
        return result(Status::Rejected,NativeBridgeCode::MissingIdentity);
    // Failed/retryable attempts never advance this watermark. The next ready
    // batch may still contain a lower, previously unsubmitted source.
    if(!changedSignature&&owner.sourceId<impl->admittedAlternating)return result(Status::Rejected,NativeBridgeCode::MissingIdentity);
    const bool duplicate=!changedSignature&&owner.sourceId==impl->admittedAlternating;
    if(duplicate&&owner!=priorOwner)return result(Status::Rejected,NativeBridgeCode::MissingIdentity);
    backend=impl->worker->status();
    if(backend.failed)return result(Status::Rejected,NativeBridgeCode::BackendFailed);
    if(backend.history_reset_pending&&backend.worker_running)return result(Status::Retry,NativeBridgeCode::HistoryResetPending);
    const auto adapted=AdaptAlternatingMainInput(device,queue,source,physicalEye,impl->alternatingPrevious[eye],
        impl->alternatingImmediate,impl->historyEpoch);
    if(!adapted)return result(Status::Rejected,adapted.code);
    if(duplicate)return result(Status::Duplicate,NativeBridgeCode::Ready);
    if(changedSignature){
        // Reset only after actual full-frame validation. This attempt belongs
        // to the old bridge epoch; retry under the new one after owner reset.
        Reset();return result(Status::Retry,NativeBridgeCode::HistoryResetPending);
    }
    if(!backend.worker_running&&impl->configuration&&!impl->startAttempted){
        impl->startAttempted=true;
        if(!impl->worker->start(device,*impl->configuration,impl->log))return result(Status::Rejected,NativeBridgeCode::BackendFailed);
        backend=impl->worker->status();
    }
    if(backend.history_reset_pending)return result(Status::Retry,NativeBridgeCode::HistoryResetPending);
    if(!backend.backend_attached||!backend.worker_running)return result(Status::Retry,NativeBridgeCode::BackendPending);
    if(!impl->worker->submit(adapted.input))return result(Status::Retry,NativeBridgeCode::InputRetry);
    impl->dormant=false;
    // Only successful worker admission advances scope, ordering or measured
    // guide history. No color ReadLease is retained by these history packets.
    impl->alternatingOwner=owner;impl->seenAlternating=impl->admittedAlternating=owner.sourceId;
    impl->resourceEpoch=source.nativeStamp.resourceEpoch;impl->spaceEpoch=source.identity.origin;impl->clockGeneration=stamp.clockGeneration;
    impl->sourceSignature=true;impl->cameraReset=source.identity.reset;impl->seenSerial=impl->admittedSerial=source.identity.serial;
    impl->alternatingPrevious[eye]=AlternatingMainGuideHistory(adapted);impl->alternatingImmediate=impl->alternatingPrevious[eye];
    return result(Status::Submitted,NativeBridgeCode::Ready,adapted.history);
}
bool NativeGenerationBridge::SelectAlternatingReady(ID3D12Device* device,const NativeAlternatingOutputContext& context,
    PresentationTime time,Selection& output) {
    Collect();
    const auto fail=[&](NativeBridgeCode code){impl->status.code=code;return false;};
    if(time.target<=0||time.period<=0||!device||!context.scope||!context.resourceEpoch||!context.captureClockGeneration||
       context.historyEpoch!=impl->historyEpoch||!impl->sourceSignature||impl->dormant||impl->sourceKind!=Impl::SourceKind::Alternating)
        return fail(NativeBridgeCode::MissingIdentity);
    Microsoft::WRL::ComPtr<IUnknown> identity;
    if(FAILED(device->QueryInterface(IID_PPV_ARGS(&identity)))||!impl->deviceIdentity||impl->deviceIdentity.Get()!=identity.Get())
        return fail(NativeBridgeCode::DeviceChanged);
    const auto& owner=impl->alternatingOwner;const auto& scope=context.scope;
    if(scope.epochs!=owner.epochs||scope.requestRevision!=owner.requestRevision||scope.latchRevision!=owner.latchRevision||
       scope.clockGeneration!=owner.clockGeneration||context.resourceEpoch!=impl->resourceEpoch||
       context.captureClockGeneration!=impl->clockGeneration||scope.epochs.hmdWorld!=impl->spaceEpoch)return fail(NativeBridgeCode::MissingIdentity);
    for(unsigned eye=0;eye<2;++eye)if(!context.expectedExtent[eye].width||!context.expectedExtent[eye].height||
       context.maximumSourceId[eye]<0||context.maximumSourceId[eye]>SourceDisplayClock::MaxExactSourceId||
       (uint64_t(context.maximumSourceId[eye])&1u)!=eye)return fail(NativeBridgeCode::MissingIdentity);
    const auto backend=impl->worker->status();
    if(backend.failed)return fail(NativeBridgeCode::BackendFailed);
    if(backend.history_reset_pending)return fail(NativeBridgeCode::HistoryResetPending);
    auto slot=std::find(impl->consumers.begin(),impl->consumers.end(),nullptr);
    if(slot==impl->consumers.end())return fail(NativeBridgeCode::ConsumerCapacity);
    // NVIDIA is an optional additional branch. The mode owner's AER scheduler
    // selects its own real base pair; this function only uses actual products
    // already selected by the existing worker, never copies MAIN to another eye.
    const auto selected=impl->worker->select(time.target,time.period);
    if(!selected.frame)return fail(NativeBridgeCode::NoSelection);
    if(selected.frame->epoch!=impl->historyEpoch||selected.frame->space_epoch!=scope.epochs.hmdWorld||
       selected.frame->cadence!=cpvr::Cadence::AlternateEyes)return fail(NativeBridgeCode::MissingIdentity);
    bool generated=false;
    for(unsigned outputEye=0;outputEye<2;++outputEye){
        const auto& value=selected.frame->eyes[outputEye];
        if(!value||value->eye!=cpvr::Eye(outputEye)||value->epoch!=impl->historyEpoch||value->space_epoch!=scope.epochs.hmdWorld||
           !value->source_id||value->source_id>uint64_t(context.maximumSourceId[outputEye])+1||
           ((value->source_id-1)&1u)!=outputEye||!Ready(value->color,device))return fail(NativeBridgeCode::MissingIdentity);
        const auto desc=value->color.texture->GetDesc();
        if(desc.Width!=context.expectedExtent[outputEye].width||desc.Height!=context.expectedExtent[outputEye].height)
            return fail(NativeBridgeCode::UnsupportedResource);
        const auto state=value->color.resting_state;
        const bool readable=value->generated?(state==D3D12_RESOURCE_STATE_COMMON||state==D3D12_RESOURCE_STATE_COPY_SOURCE):state==CaptureColorReadState;
        if(!Shape(desc)||!ColorFormat(desc.Format)||value->color.transfer!=cpvr::ColorTransfer::SrgbEncoded||!readable)return fail(NativeBridgeCode::UnsupportedResource);
        generated|=value->generated;
    }
    if(generated&&!backend.backend_attached){impl->status.code=NativeBridgeCode::BackendPending;return false;}
    auto lease=std::make_shared<NativeSelectionLease>();lease->selected=selected;lease->device=device;*slot=lease;
    output={};output.native=lease;output.synthetic=generated;
    output.kind=selected.new_presentation?(generated?Cadence::Next::Midpoint:Cadence::Next::Real):Cadence::Next::Repeat;
    output.frame.identity.serial=std::max(selected.frame->eyes[0]->source_id,selected.frame->eyes[1]->source_id);
    output.frame.identity.origin=selected.frame->space_epoch;output.frame.identity.stereo=true;
    for(unsigned outputEye=0;outputEye<2;++outputEye){
        const auto& value=*selected.frame->eyes[outputEye];output.frame.color[outputEye]=value.color.texture.Get();
        const auto& p=value.render_pose;output.frame.pose[outputEye]={{p.orientation[0],p.orientation[1],p.orientation[2],p.orientation[3]},
            {p.position[0],p.position[1],p.position[2]}};
        const auto& f=value.fov;output.frame.fov[outputEye]={f.left,f.right,f.up,f.down};
    }
    impl->status.code=NativeBridgeCode::Ready;return true;
}
bool NativeGenerationBridge::SelectAlternating(ID3D12Device* device,ID3D12CommandQueue* queue,const Frame& source,
    cpvr::Eye physicalEye,PresentationTime time,Selection& output) {
    const auto ingested=IngestAlternating(device,queue,source,physicalEye);
    const auto eye=unsigned(physicalEye);if(eye>1)return false;
    const auto& stamp=source.nativeStamp.eyes[eye];
    if(!stamp.mainEyeSource)return false;
    NativeAlternatingOutputContext context;context.scope=stamp.mainEyeSource;context.resourceEpoch=source.nativeStamp.resourceEpoch;
    context.captureClockGeneration=stamp.clockGeneration;context.historyEpoch=HistoryEpoch();
    for(unsigned i=0;i<2;++i){
        const auto& history=impl->alternatingPrevious[i];
        if(history){context.expectedExtent[i]={uint32_t(history.colorDescriptor.Width),history.colorDescriptor.Height};
            context.maximumSourceId[i]=history.sourceStamp.mainEyeSource.sourceId;}
    }
    if(source.color[eye]){const auto desc=source.color[eye]->GetDesc();
        if(desc.Width>UINT32_MAX)return false;
        context.expectedExtent[eye]={uint32_t(desc.Width),desc.Height};context.maximumSourceId[eye]=stamp.mainEyeSource.sourceId;}
    if(SelectAlternatingReady(device,context,time,output))return true;
    // Preserve the actionable input reason when no old product could be used.
    if(ingested.status==NativeAlternatingIngestStatus::Rejected||ingested.status==NativeAlternatingIngestStatus::Retry)
        impl->status.code=ingested.code;
    return false;
}
void NativeGenerationBridge::Submitted(const Selection& result,ID3D12Fence* fence,uint64_t value,
    bool copySubmitted,bool signalSucceeded,bool accepted,int64_t displayTime) {
    if(!result.native)return;
    const auto found=std::find(impl->consumers.begin(),impl->consumers.end(),result.native);
    if(found==impl->consumers.end() || result.native->submissionReported)return;
    auto& lease=*result.native;lease.submissionReported=true;lease.copySubmitted=copySubmitted;
    lease.signalSucceeded=signalSucceeded && fence && value && value!=UINT64_MAX && SameDevice(fence,lease.device.Get());
    if(lease.signalSucceeded) {lease.consumerFence=fence;lease.consumerValue=value;}
    if(copySubmitted && !lease.signalSucceeded)impl->status.code=NativeBridgeCode::ConsumerCompletionUnknown;
    auto witness=result.ownWitness;
    if(witness.attempt){OwnWitnessSubmission(witness,accepted,copySubmitted,signalSucceeded,displayTime,false,false,false);witness.signalValidatedKnown=true;witness.signalSucceeded=lease.signalSucceeded;}
    if(accepted && copySubmitted && lease.signalSucceeded && displayTime>0 && lease.selected.frame) {
        const auto& frame=*lease.selected.frame;
        const bool acknowledged=impl->worker->acknowledge(frame.pair_id,frame.epoch,frame.space_epoch,displayTime);
        if(witness.attempt){witness.ackCall=true;witness.ackReturnKnown=true;witness.ackReturn=acknowledged;}
    }
    if(witness.attempt){try{witness.submittedCounters=WitnessCounters(impl->worker->status(),witness.target);witness.submittedCounters.coherent&=witness.history==impl->historyEpoch;}catch(...){}RecordOwnWitness(witness);}
    Collect();
}
NativeReadScope::NativeReadScope(ID3D12GraphicsCommandList* commands,const Selection& selection,unsigned eye,bool pixelRead):list(commands) {
    reading=selection.synthetic ? D3D12_RESOURCE_STATE_COPY_SOURCE : CaptureColorReadState;
    if(!selection.native){
        // FidelityFX/OFA products rest in COPY_SOURCE as registered with the
        // SDK. Paired reconstruction samples them in a pixel shader instead
        // of copying them. Give that exclusive output an SRV read scope too.
        if(!selection.synthetic||!pixelRead)return;
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        if(!commands||eye>1||!selection.frame.color[eye]||commands->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT||
           FAILED(commands->GetDevice(IID_PPV_ARGS(&device)))||!SameDevice(selection.frame.color[eye],device.Get())){valid=false;return;}
        resource=selection.frame.color[eye];resting=D3D12_RESOURCE_STATE_COPY_SOURCE;reading=CaptureColorReadState;
        changed=true;Barrier(list,resource,resting,reading);return;
    }
    if(!commands || eye>1 || !selection.native->selected.frame ||
       !SameDevice(commands,selection.native->device.Get()) || commands->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT) {valid=false;return;}
    const auto& value=selection.native->selected.frame->eyes[eye];
    if(!value || value->color.texture.Get()!=selection.frame.color[eye]) {valid=false;return;}
    resource=value->color.texture.Get();resting=value->color.resting_state;
    const bool readable=value->generated ?
        (resting==D3D12_RESOURCE_STATE_COMMON || resting==D3D12_RESOURCE_STATE_COPY_SOURCE) : resting==CaptureColorReadState;
    if(!readable) {valid=false;return;}
    reading=pixelRead?CaptureColorReadState:(resting==D3D12_RESOURCE_STATE_COMMON?D3D12_RESOURCE_STATE_COPY_SOURCE:resting);
    changed=resting!=reading;
    if(changed)Barrier(list,resource,resting,reading);
    selection.native->commandsRecorded=true;
}
NativeReadScope::~NativeReadScope() {if(changed && valid)Barrier(list,resource,reading,resting);}
}

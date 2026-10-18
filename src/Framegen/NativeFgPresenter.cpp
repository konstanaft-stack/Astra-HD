#include "Framegen/NativeFgPresenter.hpp"
#include "Framegen/NativeFgOutput.hpp"
#include "Framegen/NativeGenerationBridge.hpp"
#include "Framegen/Framegen.hpp"
#include "Framegen/NativeFgPairedSource.hpp"
#include "Framegen/NativeFgObservation.hpp"
#include <algorithm>
#include <deque>
extern void Log(const char*,...);
namespace cvr::framegen {
namespace {
unsigned SelectedPolicyRevision(const cpvr::GenerationSelection& selected) {
    unsigned revision{};
    if(!selected.frame)return 0;
    for(const auto& eye:selected.frame->eyes)if(eye && eye->generated) {
        // Synthetic rows in this presenter retain their NativeFgPacket as lifetime.
        const auto* packet=static_cast<const NativeFgPacket*>(eye->color.lifetime.get());
        if(!packet || !packet->receipt.policyRevision || (revision && revision!=packet->receipt.policyRevision))return 0;
        revision=packet->receipt.policyRevision;
    }
    return revision;
}
bool SameScope(const MainEyeSourceReceipt&a,const MainEyeSourceReceipt&b){return a&&b&&a.epochs==b.epochs&&a.clockGeneration==b.clockGeneration&&a.requestRevision==b.requestRevision&&a.latchRevision==b.latchRevision;}
struct Metadata {NativeEyeStamp stamp;cpvr::EyeFrame frame;AerRealGeometry geometry;CaptureTransfer transfer{};};
Metadata Meta(const Frame&frame,unsigned eye,const AerRealGeometry&geometry,uint64_t epoch){
    Metadata m;m.stamp=frame.nativeStamp.eyes[eye];m.geometry=geometry;m.transfer=frame.nativeStamp.transfer;
    auto& f=m.frame;f.eye=cpvr::Eye(eye);f.epoch=epoch;f.space_epoch=m.stamp.spaceEpoch;f.source_id=uint64_t(m.stamp.mainEyeSource.sourceId)+1;f.content_time_ns=m.stamp.contentTimeNs;
    const auto&p=frame.pose[eye];f.render_pose={{p.orientation.x,p.orientation.y,p.orientation.z,p.orientation.w},{p.position.x,p.position.y,p.position.z}};
    const auto&v=geometry.renderedFov;f.fov={v.angleLeft,v.angleRight,v.angleUp,v.angleDown};return m;
}
bool Matches(const Metadata&m,const SrSourceStamp&s){return m.stamp.mainEyeSource==s.source&&m.stamp.poseId==s.poseId&&m.stamp.poseWriteId==s.writeId&&
    m.stamp.spaceEpoch==s.origin&&m.stamp.clockGeneration==s.captureClockGeneration&&m.stamp.engineView==s.engineView&&m.stamp.nativeFrameKnown&&s.nativeFrameKnown&&m.stamp.nativeFrame==s.nativeFrame&&m.stamp.posePredictionNs==s.locatedPoseTimeNs;}
bool Retired(const NativeSelectionLease&l){if(!l.submissionReported)return false;if(!l.copySubmitted)return true;
    if(!l.signalSucceeded||!l.consumerFence||!l.consumerValue)return false;const auto n=l.consumerFence->GetCompletedValue();return n!=UINT64_MAX&&n>=l.consumerValue;}
}
struct NativeFgPresenter::Impl {
    MainEyeSourceReceipt scope;
    uint64_t generation{},sourceGeneration{},resourceEpoch{},clock{},epoch=1;
    bool pairedMode{};uint64_t pairedWorld{},pairedOrigin{},pairedCreate{},pairedHistory{};
    std::deque<Metadata> metadata;
    cpvr::GenerationCursor cursor,proposal;
    uint64_t proposalPair{};
    std::array<std::shared_ptr<NativeSelectionLease>,8> consumers;
    uint64_t selected{},submitted{},newPairs{},repeats{},generated[2]{},missingMetadata{},rejected{},ticks{};
    uint64_t phaseAccepted[2][3]{};
    void Collect(){for(auto&c:consumers)if(c&&Retired(*c))c.reset();}
    const Metadata* Find(const SrSourceStamp&s)const{for(const auto&m:metadata)if(Matches(m,s))return &m;return nullptr;}
};
NativeFgPresenter::NativeFgPresenter():impl(std::make_unique<Impl>()){}
NativeFgPresenter::~NativeFgPresenter(){impl->Collect();
    // As with the existing NativeGenerationBridge, an unknown consumer GPU
    // completion must not free textures still referenced by recorded commands.
    for(auto&c:impl->consumers)if(c&&!Retired(*c))(void)new std::shared_ptr<NativeSelectionLease>(std::move(c));
}
void NativeFgPresenter::Reset(){impl->metadata.clear();impl->cursor={};impl->proposal={};impl->proposalPair=0;impl->scope={};++impl->epoch;impl->Collect();}
void NativeFgPresenter::Collect(){impl->Collect();}
uint64_t NativeFgPresenter::Epoch()const{return impl->epoch;}
void NativeFgPresenter::Ingest(const AerReadyIngress&batch){
    if(batch.candidates.empty())return;
    if(impl->pairedMode){Reset();impl->pairedMode=false;}
    const auto&scope=batch.candidates.front()->receipt;
    if(!SameScope(impl->scope,scope)||impl->generation!=batch.generation||impl->sourceGeneration!=batch.sourceGeneration||impl->resourceEpoch!=batch.resourceEpoch||impl->clock!=batch.captureClockGeneration){
        Reset();impl->scope=scope;impl->generation=batch.generation;impl->sourceGeneration=batch.sourceGeneration;impl->resourceEpoch=batch.resourceEpoch;impl->clock=batch.captureClockGeneration;
    }
    for(const auto*c:batch.candidates){const auto eye=unsigned(c->receipt.physicalEye);if(eye>1)continue;
        auto m=Meta(c->frame,eye,c->geometry,impl->epoch);
        if(std::none_of(impl->metadata.begin(),impl->metadata.end(),[&](const auto&known){return known.stamp.mainEyeSource==m.stamp.mainEyeSource&&known.stamp.poseId==m.stamp.poseId&&known.stamp.poseWriteId==m.stamp.poseWriteId;}))impl->metadata.push_back(std::move(m));
    }
    while(impl->metadata.size()>32)impl->metadata.pop_front();
}
bool NativeFgPresenter::Select(ID3D12Device*device,const AerRealSelection&baseline,PresentationTime time,Selection&out){
    impl->Collect();if(!device||!baseline||!SameScope(impl->scope,baseline.referenceReceipt)||impl->generation!=baseline.generation||impl->sourceGeneration!=baseline.sourceGeneration||impl->resourceEpoch!=baseline.resourceEpoch)return false;
    auto cell=std::find(impl->consumers.begin(),impl->consumers.end(),nullptr);if(cell==impl->consumers.end())return false;
    std::array<std::vector<cpvr::GenerationProduct>,2> rows;
    // Only the two current real baseline leases survive selection. Older
    // real entries are metadata, so they cannot exhaust the capture pool.
    for(unsigned eye=0;eye<2;++eye){const auto&b=*baseline.eyes[eye];auto m=Meta(b.frame,eye,b.geometry,impl->epoch);
        auto f=std::make_shared<cpvr::EyeFrame>(m.frame);f->color={b.frame.color[eye],b.frame.colorCompletion.fence,b.frame.colorCompletion.fenceValue,
            b.frame.colorCompletion.colorState,b.frame.lease};f->color.source_format=b.frame.color[eye]->GetDesc().Format;
        f->color.transfer=m.transfer==CaptureTransfer::SrgbEncoded?cpvr::ColorTransfer::SrgbEncoded:cpvr::ColorTransfer::Linear;
        rows[eye].push_back({f,{false,f->source_id,f->source_id,0,0,f->content_time_ns,f->content_time_ns,0,0}});
    }
    for(const auto&packet:CollectNativeFgOutputs()){
        if(!packet->valid->load(std::memory_order_acquire)||!SameScope(impl->scope,packet->receipt.current.source))continue;
        const auto eye=unsigned(packet->receipt.current.source.physicalEye);if(eye>1||packet->receipt.current.source.sourceId>baseline.sourceIds[eye])continue;
        const auto*a=impl->Find(packet->receipt.previous),*b=impl->Find(packet->receipt.current);if(!a||!b){++impl->missingMetadata;continue;}
        if(a->frame.epoch!=impl->epoch||b->frame.epoch!=impl->epoch||!AerRealPresenter::SameCalibration(a->geometry,b->geometry)||!AerRealPresenter::SameCalibration(b->geometry,baseline.eyes[eye]->geometry)){++impl->rejected;continue;}
        const auto transfer=b->transfer==CaptureTransfer::SrgbEncoded?cpvr::ColorTransfer::SrgbEncoded:cpvr::ColorTransfer::Linear;
        auto real=std::make_shared<cpvr::EyeFrame>(b->frame);real->color={packet->real,packet->fence,packet->value,CaptureColorReadState,std::const_pointer_cast<NativeFgPacket>(packet)};
        real->color.source_format=packet->real->GetDesc().Format;real->color.transfer=transfer;
        rows[eye].push_back({real,{false,real->source_id,real->source_id,0,0,real->content_time_ns,real->content_time_ns,0,0}});
        if(!packet->interpolationAllowed)continue;
        const auto stamp=cpvr::interpolation_stamp(a->frame,b->frame,packet->receipt.index,packet->receipt.count);if(!stamp){++impl->rejected;continue;}
        auto f=std::make_shared<cpvr::EyeFrame>(b->frame);f->generated=true;f->content_time_ns=stamp->content_time_ns;f->render_pose=stamp->render_pose;f->fov=stamp->fov;
        f->color={packet->color,packet->fence,packet->value,D3D12_RESOURCE_STATE_COMMON,std::const_pointer_cast<NativeFgPacket>(packet)};
        f->color.source_format=packet->color->GetDesc().Format;f->color.transfer=transfer;
        rows[eye].push_back({std::move(f),stamp->origin});
    }
    cpvr::GenerationPolicy policy;policy.generated_per_interval=1;policy.cadence=cpvr::Cadence::AlternateEyes;policy.presentation_pattern=cpvr::PresentationPattern::Chessboard;
    auto proposed=impl->cursor;auto selected=cpvr::choose_generation_pair(rows[0],rows[1],time.target,time.period,policy,proposed);
    if(++impl->ticks<=8||impl->ticks%180==0){const auto s=ReadNativeFgOutputStatus();Log("[NATIVE-FG-XR-SELECT] ticks=%llu rowsL=%zu rowsR=%zu metadata=%zu ready=%llu busy=%llu missing=%llu rejected=%llu pair=%d requested=%s compatible=%u skew=%u future=%u expired=%u\n",
        impl->ticks,rows[0].size(),rows[1].size(),impl->metadata.size(),s.ready,s.poolBusy,impl->missingMetadata,impl->rejected,int(bool(selected.frame)),selected.requested_step.c_str(),selected.diagnostics.compatible_pairs,selected.diagnostics.skew_pairs,selected.diagnostics.future_pairs,selected.diagnostics.expired_pairs);}
    if(!selected.frame)return false;
    auto lease=std::make_shared<NativeSelectionLease>();lease->selected=selected;lease->device=device;
    lease->observedPolicyRevision=SelectedPolicyRevision(selected);
    if(!BuildAerNativePair(baseline,lease,impl->epoch)){++impl->rejected;return false;}
    *cell=lease;impl->proposal=std::move(proposed);impl->proposalPair=selected.frame->pair_id;
    out={};out.native=lease;out.synthetic=selected.frame->eyes[0]->generated||selected.frame->eyes[1]->generated;
    out.kind=selected.new_presentation?(out.synthetic?Cadence::Next::Midpoint:Cadence::Next::Real):Cadence::Next::Repeat;
    out.frame.identity.serial=std::max(selected.frame->eyes[0]->source_id,selected.frame->eyes[1]->source_id);out.frame.identity.origin=selected.frame->space_epoch;out.frame.identity.stereo=true;
    for(unsigned eye=0;eye<2;++eye){const auto&f=*selected.frame->eyes[eye];out.frame.color[eye]=f.color.texture.Get();const auto&p=f.render_pose;
        out.frame.pose[eye]={{p.orientation[0],p.orientation[1],p.orientation[2],p.orientation[3]},{p.position[0],p.position[1],p.position[2]}};out.frame.fov[eye]={f.fov.left,f.fov.right,f.fov.up,f.fov.down};}
    ++impl->selected;return true;
}
bool NativeFgPresenter::SelectPaired(ID3D12Device*device,const Frame&baseline,uint64_t world,PresentationTime time,Selection&out){
    impl->Collect();if(!device||!world||!baseline.identity.stereo||!baseline.identity.serial||!baseline.nativeStamp.resourceEpoch)return false;
    if(!impl->pairedMode||impl->pairedWorld!=world||impl->pairedOrigin!=baseline.identity.origin||impl->resourceEpoch!=baseline.nativeStamp.resourceEpoch){
        Reset();impl->pairedMode=true;impl->pairedWorld=world;impl->pairedOrigin=baseline.identity.origin;impl->resourceEpoch=baseline.nativeStamp.resourceEpoch;
        impl->pairedCreate=impl->pairedHistory=0;
    }
    const auto packets=CollectNativeFgOutputs();uint64_t create{},history{};
    for(const auto& p:packets){const auto&r=p->receipt;
        if(r.paired&&r.gameWorldEpoch==world&&r.resourceEpoch==baseline.nativeStamp.resourceEpoch&&r.current.origin==baseline.identity.origin&&r.currentPairSerial<=baseline.identity.serial){
            if(r.createOrdinal>create){create=r.createOrdinal;history=r.historyEpoch;}else if(r.createOrdinal==create)history=std::max(history,r.historyEpoch);}}
    if(!create)return false;
    if(create!=impl->pairedCreate||history!=impl->pairedHistory){Reset();impl->pairedCreate=create;impl->pairedHistory=history;}
    auto cell=std::find(impl->consumers.begin(),impl->consumers.end(),nullptr);if(cell==impl->consumers.end())return false;
    std::array<std::vector<cpvr::GenerationProduct>,2> rows;
    std::array<std::vector<std::pair<uint64_t,std::shared_ptr<const Frame>>>,2> metadata;
    unsigned count=1;
    unsigned phaseRows[2][3]{};
    uint64_t minimumRowClock=UINT64_MAX,maximumRowClock{};
    const auto sameGeometry=[](const AerRealGeometry&a,const AerRealGeometry&b){
        if(a.width!=b.width||a.height!=b.height||a.crop!=b.crop)return false;
        const auto& x=a.renderedFov;const auto& y=b.renderedFov;
        return std::abs(x.angleLeft-y.angleLeft)<.00002f&&std::abs(x.angleRight-y.angleRight)<.00002f&&
            std::abs(x.angleUp-y.angleUp)<.00002f&&std::abs(x.angleDown-y.angleDown)<.00002f;};
    for(const auto& packet:packets){const auto&r=packet->receipt;const auto eye=r.pairedEye;
        if(!r.paired||eye>1||!ValidNativeFgReceipt(r)||!packet->valid->load(std::memory_order_acquire)||
           r.createOrdinal!=create||r.historyEpoch!=history||r.gameWorldEpoch!=world||r.resourceEpoch!=baseline.nativeStamp.resourceEpoch||
           r.current.origin!=baseline.identity.origin||r.currentPairSerial>baseline.identity.serial||!r.pairedPreviousMetadata||!r.pairedCurrentMetadata)continue;
        const auto& a=*r.pairedPreviousMetadata;const auto& b=*r.pairedCurrentMetadata;
        if(!MatchesPairedNativeStamp(a.nativeStamp.eyes[eye],r.previous)||!MatchesPairedNativeStamp(b.nativeStamp.eyes[eye],r.current)||
           a.nativeStamp.renderPublicationTimeNs<=0||a.nativeStamp.renderPublicationTimeNs>=b.nativeStamp.renderPublicationTimeNs||
           a.nativeStamp.publicationClockGeneration!=b.nativeStamp.publicationClockGeneration||
           !a.projection[eye]||!b.projection[eye]||!baseline.projection[eye])continue;
        const auto desc=packet->real->GetDesc();AerRealGeometry ga,gb,gbase;
        ga.width=gb.width=gbase.width=uint32_t(desc.Width);ga.height=gb.height=gbase.height=desc.Height;
        if(!AerRealPresenter::ResolveCapturedGeometry(a.projection[eye]->constants,a.nativeStamp.eyes[eye].referenceOptics,eye,ga)||
           !AerRealPresenter::ResolveCapturedGeometry(b.projection[eye]->constants,b.nativeStamp.eyes[eye].referenceOptics,eye,gb)||
           !AerRealPresenter::ResolveCapturedGeometry(baseline.projection[eye]->constants,baseline.nativeStamp.eyes[eye].referenceOptics,eye,gbase)||
           !sameGeometry(ga,gb)||!sameGeometry(gb,gbase))continue;
        const auto meta=[&](const Frame&f){cpvr::EyeFrame value;value.eye=cpvr::Eye(eye);value.epoch=impl->epoch;value.space_epoch=f.identity.origin;
            value.source_id=f.identity.serial;value.stereo_pair_id=f.identity.serial;value.content_time_ns=f.nativeStamp.renderPublicationTimeNs;
            const auto&p=f.pose[eye];value.render_pose={{p.orientation.x,p.orientation.y,p.orientation.z,p.orientation.w},{p.position.x,p.position.y,p.position.z}};
            const auto&v=f.fov[eye];value.fov={v.angleLeft,v.angleRight,v.angleUp,v.angleDown};return value;};
        const auto old=meta(a);auto real=std::make_shared<cpvr::EyeFrame>(meta(b));
        real->color={packet->real,packet->fence,packet->value,CaptureColorReadState,std::const_pointer_cast<NativeFgPacket>(packet)};
        real->color.source_format=desc.Format;real->color.transfer=cpvr::ColorTransfer::SrgbEncoded;
        minimumRowClock=(std::min)(minimumRowClock,b.nativeStamp.publicationClockGeneration);
        maximumRowClock=(std::max)(maximumRowClock,b.nativeStamp.publicationClockGeneration);
        rows[eye].push_back({real,{false,real->source_id,real->source_id,0,0,real->content_time_ns,real->content_time_ns,0,0}});
        metadata[eye].push_back({real->source_id,r.pairedCurrentMetadata});count=std::max(count,r.count);
        if(!packet->interpolationAllowed)continue;
        const auto stamp=cpvr::interpolation_stamp(old,*real,r.index,r.count);if(!stamp)continue;
        auto generated=std::make_shared<cpvr::EyeFrame>(*real);generated->generated=true;generated->content_time_ns=stamp->content_time_ns;
        generated->render_pose=stamp->render_pose;generated->fov=stamp->fov;
        generated->color={packet->color,packet->fence,packet->value,D3D12_RESOURCE_STATE_COMMON,std::const_pointer_cast<NativeFgPacket>(packet)};
        generated->color.source_format=packet->color->GetDesc().Format;generated->color.transfer=cpvr::ColorTransfer::SrgbEncoded;
        rows[eye].push_back({std::move(generated),stamp->origin});
        ++phaseRows[eye][r.index-1];
    }
    cpvr::GenerationPolicy policy;policy.generated_per_interval=count;policy.cadence=cpvr::Cadence::StereoPairs;
    policy.presentation_pattern=cpvr::PresentationPattern::Custom;policy.custom_sequence="RR";
    for(unsigned i=0;i<count;++i)policy.custom_sequence+=",GG";
    auto proposed=impl->cursor;auto selected=cpvr::choose_generation_pair(rows[0],rows[1],time.target,time.period,policy,proposed);
    if(++impl->ticks<=8||impl->ticks%180==0)Log("[PAIRED-FG-SELECT] rows=%zu/%zu selected=%d create=%llu history=%llu compatible=%u skew=%u future=%u expired=%u request=%s\n",
        rows[0].size(),rows[1].size(),int(bool(selected.frame)),create,history,selected.diagnostics.compatible_pairs,selected.diagnostics.skew_pairs,
        selected.diagnostics.future_pairs,selected.diagnostics.expired_pairs,selected.requested_step.c_str());
    if(impl->ticks<=8||impl->ticks%180==0){const auto output=ReadNativeFgOutputStatus();unsigned held{},unreported{},pending{},unknown{};
        for(const auto&c:impl->consumers)if(c){++held;unreported+=!c->submissionReported;
            if(c->copySubmitted&&!c->signalSucceeded)++unknown;
            if(c->signalSucceeded&&c->consumerFence&&c->consumerFence->GetCompletedValue()<c->consumerValue)++pending;}
        Log("[PAIRED-FG-FLOW] capture=%llu ready=%llu suppressed=%llu busy=%llu unknown=%llu held=%u unreported=%u pending=%u unknownConsumer=%u realAgeMs=%.3f/%.3f genAgeMs=%.3f/%.3f regressed=%u early=%u count=%u target=%lld\n",
            output.recorded,output.ready,output.suppressed,output.poolBusy,output.unknown,held,unreported,pending,unknown,
            selected.diagnostics.newest_real_ns[0]?(time.target-selected.diagnostics.newest_real_ns[0])*1e-6:-1,
            selected.diagnostics.newest_real_ns[1]?(time.target-selected.diagnostics.newest_real_ns[1])*1e-6:-1,
            selected.diagnostics.newest_generated_ns[0]?(time.target-selected.diagnostics.newest_generated_ns[0])*1e-6:-1,
            selected.diagnostics.newest_generated_ns[1]?(time.target-selected.diagnostics.newest_generated_ns[1])*1e-6:-1,
            selected.diagnostics.regressed_pairs,selected.diagnostics.early_for_slot,count,static_cast<long long>(time.target));
        const bool clockKnown=time.observedNow>0&&time.observedClockGeneration&&
            minimumRowClock==maximumRowClock&&minimumRowClock==time.observedClockGeneration;
        const auto ageNow=[&](int64_t stamp){return clockKnown&&stamp>0?(time.observedNow-stamp)*1e-6:-1.;};
        Log("[PAIRED-FG-CLOCK] known=%d now=%lld target=%lld period=%lld observedClock=%llu rowsClock=%llu..%llu baselineClock=%llu predictionLeadMs=%.3f realAgeNowMs=%.3f/%.3f genAgeNowMs=%.3f/%.3f maxAgeMs=%.3f\n",
            int(clockKnown),time.observedNow,time.target,time.period,time.observedClockGeneration,minimumRowClock,maximumRowClock,
            baseline.nativeStamp.publicationClockGeneration,clockKnown?(time.target-time.observedNow)*1e-6:-1.,
            ageNow(selected.diagnostics.newest_real_ns[0]),ageNow(selected.diagnostics.newest_real_ns[1]),
            ageNow(selected.diagnostics.newest_generated_ns[0]),ageNow(selected.diagnostics.newest_generated_ns[1]),policy.maximum_age_ns*1e-6);
        for(unsigned eye=0;eye<2;++eye)Log("[PAIRED-FG-PHASE] eye=%u recorded=%llu/%llu/%llu ready=%llu/%llu/%llu cancelled=%llu/%llu/%llu accepted=%llu/%llu/%llu rows=%u/%u/%u\n",
            eye,output.phaseRecorded[eye][0],output.phaseRecorded[eye][1],output.phaseRecorded[eye][2],
            output.phaseReady[eye][0],output.phaseReady[eye][1],output.phaseReady[eye][2],
            output.phaseCancelled[eye][0],output.phaseCancelled[eye][1],output.phaseCancelled[eye][2],
            impl->phaseAccepted[eye][0],impl->phaseAccepted[eye][1],impl->phaseAccepted[eye][2],
            phaseRows[eye][0],phaseRows[eye][1],phaseRows[eye][2]);
    }
    if(!selected.frame)return false;
    Selection candidate;candidate.frame.nativeStamp.resourceEpoch=baseline.nativeStamp.resourceEpoch;candidate.frame.nativeStamp.transfer=CaptureTransfer::SrgbEncoded;
    for(unsigned eye=0;eye<2;++eye){const auto& value=*selected.frame->eyes[eye];
        const auto found=std::find_if(metadata[eye].begin(),metadata[eye].end(),[&](const auto&m){return m.first==value.source_id;});
        if(found==metadata[eye].end())return false;const auto& src=*found->second;
        candidate.frame.nativeStamp.eyes[eye]=src.nativeStamp.eyes[eye];candidate.frame.projection[eye]=src.projection[eye];
        candidate.frame.engineView[eye]=src.engineView[eye];candidate.frame.color[eye]=value.color.texture.Get();
        const auto&p=value.render_pose;candidate.frame.pose[eye]={{p.orientation[0],p.orientation[1],p.orientation[2],p.orientation[3]},{p.position[0],p.position[1],p.position[2]}};
        candidate.frame.fov[eye]={value.fov.left,value.fov.right,value.fov.up,value.fov.down};candidate.synthetic|=value.generated;
        candidate.frame.identity.pose[eye]=value.generated?0:src.identity.pose[eye];
    }
    candidate.frame.identity.serial=std::max(selected.frame->eyes[0]->source_id,selected.frame->eyes[1]->source_id);
    candidate.frame.identity.origin=selected.frame->space_epoch;candidate.frame.identity.stereo=true;
    auto lease=std::make_shared<NativeSelectionLease>();lease->selected=selected;lease->device=device;candidate.native=lease;
    lease->observedPolicyRevision=SelectedPolicyRevision(selected);lease->observedPairedMode=true;
    candidate.kind=selected.new_presentation?(candidate.synthetic?Cadence::Next::Midpoint:Cadence::Next::Real):Cadence::Next::Repeat;
    *cell=lease;impl->proposal=std::move(proposed);impl->proposalPair=selected.frame->pair_id;out=std::move(candidate);++impl->selected;return true;
}
void NativeFgPresenter::Submitted(const Selection&s,ID3D12Fence*fence,uint64_t value,bool accepted,bool copied,bool signaled,int64_t displayTime){
    if(!s.native)return;const auto cell=std::find(impl->consumers.begin(),impl->consumers.end(),s.native);if(cell==impl->consumers.end()||s.native->submissionReported)return;
    auto&l=*s.native;l.submissionReported=true;l.copySubmitted=copied;l.signalSucceeded=signaled&&fence&&value&&value!=UINT64_MAX;
    if(l.signalSucceeded){l.consumerFence=fence;l.consumerValue=value;}
    if(accepted&&copied&&l.signalSucceeded&&displayTime>0&&l.selected.frame&&l.selected.frame->pair_id==impl->proposalPair){
        if(l.selected.new_presentation){auto committed=impl->proposal;
            if(impl->cursor.valid&&impl->cursor.display_time_ns>0){const auto lag=committed.display_time_ns-committed.content_clock_ns;committed.content_clock_ns=displayTime-lag;}
            committed.display_time_ns=displayTime;impl->cursor=std::move(committed);++impl->newPairs;
            for(unsigned eye=0;eye<2;++eye)if(l.selected.frame->eyes[eye]->generated){
                ++impl->generated[eye];const auto index=l.selected.origins[eye].index;
                if(l.observedPairedMode&&index>=1&&index<=3)++impl->phaseAccepted[eye][index-1];
            }
        }else ++impl->repeats;
        ++impl->submitted;
        if(MetricsEnabled())OnSubmitted(s.frame.identity.serial,s.synthetic,!l.selected.new_presentation,NowMs());
        if(s.synthetic && l.selected.new_presentation)OnNativeGeneratedSubmitted(l.observedPolicyRevision,
            l.observedPairedMode ? PresentedCamera::Paired : PresentedCamera::Alternating,NowMs());
        if(impl->submitted<=8||impl->submitted%120==0)Log("[NATIVE-FG-XR] accepted=%llu newPairs=%llu repeats=%llu generatedL=%llu generatedR=%llu pair=%llu epoch=%llu display=%lld\n",impl->submitted,impl->newPairs,impl->repeats,impl->generated[0],impl->generated[1],l.selected.frame->pair_id,impl->epoch,static_cast<long long>(displayTime));
    }
    impl->proposalPair=0;impl->Collect();
}
}

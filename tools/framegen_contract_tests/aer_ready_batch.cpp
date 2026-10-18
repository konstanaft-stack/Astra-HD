#define main PriorAerFixtureMain
#include "aer_real_presenter.cpp"
#undef main

static AerRealCandidate Candidate(Fixture& f,int64_t id,uint64_t latch=6,float projection=1.f){
    auto frame=f.Make(id);const auto eye=unsigned(id&1);
    frame.nativeStamp.eyes[eye].mainEyeSource.latchRevision=latch;
    auto input=Edit(frame,eye);input->camera.constants.values[0]=projection;input->submitted=input->hasDepth=true;
    auto converted=AerRealPresenter::FromMainFrame(std::move(frame),AerV2Eye(eye));
    Check(bool(converted),"ready batch factory rejected valid source");return std::move(*converted.candidate);
}
static AerRealSelection Batch(Fixture& f,AerRealPresenter& p,AerRealTick& tick,std::initializer_list<int64_t> ids){
    std::vector<AerRealCandidate> ready;for(auto id:ids)ready.push_back(Candidate(f,id));
    auto result=p.StepReady({++tick.frame,tick.appliedDelay,tick.sourceUpper,tick.generation,tick.epochs},ready);
    for(const auto& item:ready)Check(!item.frame.lease&&!item.frame.inputs[0]&&!item.frame.inputs[1],"StepReady did not consume caller batch");
    return result;
}
static void OriginalQueueOrderAndRetainedBacklog(){
    // Luke26.03 306DD0 (mode0) calls307630 once for R, then once for L.
    // 307630 walks the queued ring and accepts the FIRST eligible ready source.
    // It retains later pending images; they are not discarded after one tick.
    // Input enumeration order is not an authoritative producer sequence.
    Fixture f;AerRealPresenter p;AerRealTick tick{-1,0,-1,5,{1,2,3}};
    auto selected=Batch(f,p,tick,{5,2,3,4,1});
    Check(selected&&selected.sourceIds==std::array<int64_t,2>{2,1},
        "Luke26.03 FIFO mismatch: first admissible ready pair must be L2/R1, not newest L4/R5");
    auto gpu=selected.OwnerForEye(1);
    selected=Batch(f,p,tick,{});
    Check(selected&&selected.sourceIds==std::array<int64_t,2>{4,3},
        "original pending L4/R3 pair was lost between display ticks");
    selected=Batch(f,p,tick,{6});
    Check(selected&&selected.sourceIds==std::array<int64_t,2>{6,5},
        "retained R5 did not join its subsequent L6 source");
    selected={};p.Reset();
    Check(f.Pins()==1,"reset leaked queued sources or released the actual outstanding GPU owner");
    gpu.reset();Check(f.Pins()==0,"FIFO queue test retained a source after all owners retired");
}
static void ProjectionOwnsNoGuideRecord(){
    Fixture f;auto frame=f.Make(1);auto input=Edit(frame,1);input->submitted=input->hasDepth=true;
    std::weak_ptr<const Inputs> weak=input;frame.projection[1]=SnapshotFrameProjection(*input);input.reset();
    auto result=AerRealPresenter::FromMainFrame(std::move(frame),AerV2Eye::Right);
    Check(bool(result)&&weak.expired(),"baseline candidate retains one of four guide Records");
    Check(result.candidate->frame.projection[1].has_value()&&!result.candidate->frame.inputs[1]&&!result.candidate->frame.identity.inputs,"projection metadata impersonates full guides");
    const auto& metadata=*result.candidate->frame.projection[1];
    Check(metadata.srInputKnown&&metadata.srInputWidth==100&&metadata.srInputHeight==50,"SR descriptor provenance not copied");
    auto frame2=f.Make(2);frame2.projection[0]=SnapshotFrameProjection(*frame2.inputs[0]);frame2.inputs[0].reset();
    Check(bool(AerRealPresenter::FromMainFrame(std::move(frame2),AerV2Eye::Left)),"catalogue projection-only source rejected");
    auto bad=f.Make(4);bad.projection[0]=SnapshotFrameProjection(*bad.inputs[0]);bad.projection[0]->poseWriteId++;
    Check(AerRealPresenter::FromMainFrame(std::move(bad),AerV2Eye::Left).code==AerRealCode::UnsupportedProjection,"invalid snapshot fell back to unrelated full guides");
    auto missing=f.Make(6);missing.inputs[0].reset();Check(!AerRealPresenter::FromMainFrame(std::move(missing),AerV2Eye::Left),"missing snapshot and guides fabricated projection");
}
static void SkippedSourcesReleaseObsoleteFifoHeads(){
    Fixture f;AerRealPresenter p;AerRealTick tick{-1,0,-1,5,{1,2,3}};
    auto selected=Batch(f,p,tick,{0,1,2,4});
    Check(selected.sourceIds==std::array<int64_t,2>{0,1}&&p.QueuedEyeCount(0)==2,"skipped-source fixture did not fill the left ring");
    selected=Batch(f,p,tick,{7});
    Check(p.QueuedEyeCount(0)==0&&f.Pins()==2,"sources below the new LEFT lower bound still occupy its producer ring");
    selected=Batch(f,p,tick,{8});
    Check(selected.sourceIds==std::array<int64_t,2>{8,7},"new compatible left could not replace discarded FIFO heads");
    p.Reset();selected={};Check(f.Pins()==0,"skipped-source fixture leaked owners");
}
static void QueueScopeCapacityAndFailure(){
    Fixture f;AerRealPresenter p;AerRealTick tick{-1,0,-1,5,{1,2,3}};
    auto selected=Batch(f,p,tick,{1,2,3,4,5});
    Check(p.QueuedEyeCount(0)==1&&p.QueuedEyeCount(1)==2,"initial original queue depth changed");
    bool threw=false;std::array<AerRealCandidate,1> offered{Candidate(f,6)};
    try{p.StepReady({++tick.frame,0,-1,5,{1,2,3}},offered,[](const AerReadyIngress&){throw std::runtime_error("downstream failure");});}
    catch(const std::runtime_error&){threw=true;}
    Check(threw&&p.QueuedEyeCount(0)==1&&p.QueuedEyeCount(1)==2&&f.Pins()==5,"downstream exception lost or duplicated the old FIFO");
    auto oldGpu=selected.OwnerForEye(1);auto scope=selected.referenceReceipt;selected={};
    p.ConstrainSourceScope(scope,11);
    Check(p.QueuedEyeCount(0)==1&&p.QueuedEyeCount(1)==2,"same source scope erased pending images");
    ++scope.latchRevision;p.ConstrainSourceScope(scope,11);
    Check(p.HeldEyeCount()==0&&p.QueuedEyeCount(0)==0&&p.QueuedEyeCount(1)==0&&f.Pins()==1,
          "stale queue was not revoked without needing a replacement capture");
    oldGpu.reset();Check(f.Pins()==0,"scope revocation released or leaked the external GPU lease");
    selected=Batch(f,p,tick,{1,2});
    selected=Batch(f,p,tick,{4,6,8,10,12,14});
    Check(p.QueuedEyeCount(0)==2&&p.QueuedEyeCount(1)==0&&f.Pins()==4,"ready queue exceeded three total slots per eye");
    selected=Batch(f,p,tick,{3});Check(selected.sourceIds==std::array<int64_t,2>{4,3},"capacity handling replaced the oldest pending source");
    p.Reset();selected={};Check(f.Pins()==0,"capacity fixture leaked source leases");
}
static void FourGuideRecordsRemainReusable(){
    Fixture f;AerRealPresenter p;AerRealTick tick{-1,0,-1,5,{1,2,3}};
    std::array<std::shared_ptr<Inputs>,4> records;for(auto& record:records)record=std::make_shared<Inputs>();
    AerRealSelection selected;
    for(int64_t id=1;id<=81;++id){
        const auto eye=unsigned(id&1),slot=unsigned(id%4);Check(records[slot].use_count()==1,"baseline AER exhausted a four-Record guide pool");
        auto frame=f.Make(id);*records[slot]=*frame.inputs[eye];records[slot]->submitted=records[slot]->hasDepth=true;
        frame.inputs[eye]=records[slot];frame.projection[eye]=SnapshotFrameProjection(*records[slot]);
        frame.inputs[eye].reset(); // Producer catalogue payload contains no guide Record.
        auto converted=AerRealPresenter::FromMainFrame(std::move(frame),AerV2Eye(eye));Check(bool(converted),"value-metadata catalogue candidate rejected");
        if(id<=2||(id&1))selected=p.Step({++tick.frame,0,-1,5,{1,2,3}},std::move(converted.candidate));
    }
    Check(selected.sourceIds==std::array<int64_t,2>{2,81},"guide retention fixture changed its fixed-left workload");
    Check(selected.eyes[0]->frame.projection[0]->frameId==702&&records[2]->frameId!=702,"reusing a guide Record mutated the retained projection value");
    for(const auto& record:records)Check(record.use_count()==1,"retained AER selection owns a guide Record");
    p.Reset();selected={};Check(f.Pins()==0,"guide pool fixture leaked color pins");
}
static void OriginalWindowsConsumeBothEyes(){
    Fixture f;AerRealPresenter p;AerRealTick tick{-1,0,-1,5,{1,2,3}};
    auto selected=Batch(f,p,tick,{2,1});Check(selected&&selected.sourceIds==std::array<int64_t,2>{2,1},"initial ready stereo batch rejected");
    selected=Batch(f,p,tick,{6,4,5});
    Check(selected&&selected.sourceIds==std::array<int64_t,2>{4,5}&&selected.newlyAccepted[0]&&selected.newlyAccepted[1],"callbacks failed to pick first eligible R then L");
    Check(selected.referenceReceipt.sourceId==4,"reference is not last accepted callback");
    selected=Batch(f,p,tick,{12,8,9});
    Check(selected.sourceIds==std::array<int64_t,2>{8,9},"newest-too-new left prevented eligible older ready left");
    Check(selected.referenceReceipt.sourceId==8,"reference incorrectly uses highest source ID");
    Check(f.Pins()==3&&p.QueuedEyeCount(0)==1,"future L12 source was lost or an obsolete source was retained");
    p.Reset();selected={};Check(f.Pins()==0,"ready batch leaked captures");
}
static void LateReadyLeftRegression(){
    Fixture f;AerRealPresenter p;AerRealTick tick{-1,0,-1,8,{1,2,3}};auto selected=Batch(f,p,tick,{1,2});
    const auto generation=selected.sourceGeneration;unsigned acceptedLeft=0,acceptedRight=0;
    // Production latest-only RED fixture ends L2/R1001. Here a completed left
    // publication survives in the producer catalogue, without a pending pin.
    for(int64_t right=3;right<=1001;right+=2){
        const int64_t left=right-1;auto leftFrame=f.Make(left);
        const uint64_t fenceValue=1000+uint64_t(right);leftFrame.colorCompletion=CaptureCompletion::AfterSignal(f.fence.Get(),fenceValue,S_OK,{},0);
        f.fence->completed=fenceValue-1;
        Check(AerRealPresenter::FromMainFrame(leftFrame,AerV2Eye::Left).code==AerRealCode::CapturePending,"pending source was accepted");
        f.fence->completed=fenceValue;
        auto leftCandidate=AerRealPresenter::FromMainFrame(std::move(leftFrame),AerV2Eye::Left);Check(bool(leftCandidate),"completed left did not become eligible metadata");
        std::array<AerRealCandidate,2> ready{Candidate(f,right),std::move(*leftCandidate.candidate)};
        selected=p.StepReady({++tick.frame,0,-1,8,{1,2,3}},ready);
        Check(selected.sourceIds==std::array<int64_t,2>{left,right},"bounded-ready batch reproduced stale L2/R1001 gap");
        Check(selected.sourceGeneration==generation&&selected.calibrationRevision==1,"readiness changed scope/calibration");
        acceptedLeft+=selected.newlyAccepted[0];acceptedRight+=selected.newlyAccepted[1];
    }
    Check(selected.sourceIds==std::array<int64_t,2>{1000,1001}&&acceptedLeft==499&&acceptedRight==500,"long sequence did not drain both eyes");
    std::cout<<"ready_batch_regression lastL=1000 lastR=1001 leftAdmissions="<<acceptedLeft<<" rightAdmissions="<<acceptedRight<<'\n';
    p.Reset();selected={};Check(f.Pins()==0,"late-ready fixture leaked pins");
}
static void TransitionsBeforePolicy(){
    Fixture f;AerRealPresenter p;AerRealTick tick{-1,0,-1,5,{1,2,3}};auto selected=Batch(f,p,tick,{1,2});
    auto gpu=selected.OwnerForEye(0);const auto oldGeneration=selected.sourceGeneration;
    std::array<AerRealCandidate,3> mixed{Candidate(f,8,6),Candidate(f,9,7),Candidate(f,10,7)};
    selected=p.StepReady({++tick.frame,0,-1,5,{1,2,3}},mixed);
    Check(selected&&selected.sourceIds==std::array<int64_t,2>{10,9}&&selected.sourceGeneration==oldGeneration+1,"scope reset occurred inside callbacks or old scope mixed");
    const auto beforeCalibration=selected.calibrationRevision;
    std::array<AerRealCandidate,3> geometry{Candidate(f,11,7),Candidate(f,12,7,1.1f),Candidate(f,10,7)};
    selected=p.StepReady({++tick.frame,0,-1,5,{1,2,3}},geometry);
    Check(!selected&&p.HeldEyeCount()==0&&selected.calibrationRevision==beforeCalibration+1,"calibration transition accepted pre-transition opposite eye");
    std::array<AerRealCandidate,2> fresh{Candidate(f,12,7,1.1f),Candidate(f,13,7)};
    selected=p.StepReady({++tick.frame,0,-1,5,{1,2,3}},fresh);
    Check(selected&&selected.sourceIds==std::array<int64_t,2>{12,13}&&selected.calibrationRevision==beforeCalibration+1,"ready pair could not recover after preflight reset");
    const auto stableGeneration=selected.sourceGeneration;
    std::array<AerRealCandidate,1> old{Candidate(f,20,6)};
    selected=p.StepReady({++tick.frame,0,-1,5,{1,2,3}},old);
    Check(selected&&selected.sourceGeneration==stableGeneration&&selected.sourceIds==std::array<int64_t,2>{12,13},"stale scope reset current pair");
    p.Reset();selected={};Check(f.Pins()==1,"reset released actual outstanding GPU source owner");gpu.reset();Check(f.Pins()==0,"GPU source owner did not retire");
}
static void InvalidBatchAndBound(){
    Fixture f;AerRealPresenter p;AerRealTick tick{-1,0,-1,5,{1,2,3}};auto selected=Batch(f,p,tick,{1,2});
    auto good=Candidate(f,3);good.frame.projection[1]->frameId++;
    std::array<AerRealCandidate,1> invalid{std::move(good)};selected=p.StepReady({++tick.frame,0,-1,5,{1,2,3}},invalid);
    Check(selected&&selected.sourceIds==std::array<int64_t,2>{2,1}&&f.Pins()==2,"invalid guide identity entered ready history or retained pin");
    std::vector<AerRealCandidate> oversized;for(int64_t id=3;id<12;++id)oversized.push_back(Candidate(f,id));
    const auto state=p.PolicyState();auto rejected=p.StepReady({++tick.frame,0,-1,5,{1,2,3}},oversized);
    Check(rejected.code==AerRealCode::InvalidInput&&p.PolicyState().lastFrame==state.lastFrame,"oversized batch mutated policy");
    Check(bool(oversized[0].frame.lease),"early rejected batch was moved despite caller ownership contract");oversized.clear();
    std::array<AerRealCandidate,1> duplicateTick{Candidate(f,3,6,1.1f)};
    rejected=p.StepReady({state.lastFrame,0,-1,5,{1,2,3}},duplicateTick);
    Check(rejected.code==AerRealCode::InvalidInput&&p.PolicyState().sourceIds==state.sourceIds,"duplicate tick reset calibration/policy");duplicateTick[0]={};
    std::array<AerRealCandidate,2> incomparable{Candidate(f,3),Candidate(f,4)};
    incomparable[0].frame.nativeStamp.resourceEpoch=8;incomparable[1].frame.nativeStamp.eyes[0].mainEyeSource.clockGeneration=24;
    // Rebuild attests the actual Frame receipt, not this public candidate copy.
    incomparable[1].receipt.clockGeneration=24;
    selected=p.StepReady({++tick.frame,0,-1,5,{1,2,3}},incomparable);
    Check(selected&&selected.candidateCode==AerRealCode::EpochMismatch&&selected.sourceIds==state.sourceIds,"incomparable domains mixed or reset a valid pair");
    p.Reset();selected={};Check(f.Pins()==0,"rejected batch ownership leaked");
}
static void OrderedBorrowedIngress(){
    Fixture f;AerRealPresenter p;AerRealTick tick{-1,0,-1,5,{1,2,3}};
    auto selected=Batch(f,p,tick,{1,2});const auto before=p.PolicyState();
    std::array<AerRealCandidate,5> ready{Candidate(f,9),Candidate(f,8),Candidate(f,12),Candidate(f,10),Candidate(f,8)};
    ready[4].frame.identity.serial+=100;ready[4].frame.nativeStamp.eyes[0].sourceSerial=ready[4].frame.identity.serial;
    const auto duplicateSerial=ready[4].frame.identity.serial;
    unsigned calls=0;std::vector<int64_t> seen;
    selected=p.StepReady({++tick.frame,0,-1,5,{1,2,3}},ready,[&](const AerReadyIngress& ingress){
        ++calls;Check(p.PolicyState().sourceIds==before.sourceIds&&p.PolicyState().lastFrame==before.lastFrame,"ingress ran after display policy");
        Check(ingress.generation==5&&ingress.sourceGeneration==selected.sourceGeneration&&ingress.resourceEpoch==7&&
              ingress.requestRevision==4&&ingress.latchRevision==6&&ingress.sourceClockGeneration==23&&ingress.captureClockGeneration==11&&
              ingress.epochs==MainEyeSourceEpochs{1,2,3},"ingress scope unavailable before ingestion");
        for(const auto* candidate:ingress.candidates){
            seen.push_back(candidate->receipt.sourceId);const auto eye=unsigned(candidate->receipt.physicalEye);
            Check(candidate->frame.projection[eye].has_value()&&!candidate->frame.inputs[0]&&!candidate->frame.inputs[1],"ingress retained native guide Records");
            if(candidate->receipt.sourceId==8)Check(candidate->frame.identity.serial==duplicateSerial,"ingress duplicate did not select newest capture serial");
        }
    });
    Check(calls==1&&seen==std::vector<int64_t>({8,9,10,12}),"ingress is not ascending/unique or omitted non-displayed ready source");
    Check(selected.sourceIds==std::array<int64_t,2>{8,9}&&selected.referenceReceipt.sourceId==8,"observer changed original FIFO R then L display behavior");
    Check(f.Pins()==4&&p.QueuedEyeCount(0)==2,"future L10/L12 backlog was lost or duplicated");p.Reset();selected={};Check(f.Pins()==0,"ingress leaked source owners");
}
static void IngressAfterScopeAndCalibration(){
    Fixture f;AerRealPresenter p;AerRealTick tick{-1,0,-1,5,{1,2,3}};auto selected=Batch(f,p,tick,{1,2});
    const auto oldGeneration=selected.sourceGeneration;uint64_t tapGeneration=0;unsigned callbacks=0;
    std::array<AerRealCandidate,4> ready{Candidate(f,9,7),Candidate(f,8,7),Candidate(f,20,6),Candidate(f,10,7)};
    ready[3].frame.projection[0]->poseWriteId++;
    selected=p.StepReady({++tick.frame,0,-1,5,{1,2,3}},ready,[&](const AerReadyIngress& ingress){
        ++callbacks;tapGeneration=ingress.sourceGeneration;
        Check(ingress.sourceGeneration==oldGeneration+1&&ingress.latchRevision==7&&p.HeldEyeCount()==0,"scope reset did not precede ingress");
        Check(ingress.candidates.size()==2&&ingress.candidates[0]->receipt.sourceId==8&&ingress.candidates[1]->receipt.sourceId==9,"stale/invalid candidate reached ingress");
    });
    Check(callbacks==1&&selected.sourceGeneration==tapGeneration&&selected.sourceIds==std::array<int64_t,2>{8,9},"preflight scope changed after ingress");
    const auto revision=selected.calibrationRevision;const auto generation=selected.sourceGeneration;
    std::array<AerRealCandidate,3> geometry{Candidate(f,11,7),Candidate(f,12,7,1.1f),Candidate(f,13,7)};
    selected=p.StepReady({++tick.frame,0,-1,5,{1,2,3}},geometry,[&](const AerReadyIngress& ingress){
        Check(ingress.calibrationRevision==revision+1&&ingress.sourceGeneration==generation+1&&p.HeldEyeCount()==0,"calibration reset did not precede ingress");
        Check(ingress.candidates.size()==2&&ingress.candidates[0]->receipt.sourceId==12&&ingress.candidates[1]->receipt.sourceId==13,"pre-calibration source reached ingress");
    });
    Check(selected.sourceIds==std::array<int64_t,2>{12,13},"calibration observer changed display admission");
    p.Reset();selected={};Check(f.Pins()==0,"scope/calibration ingress leaked owners");
}
static void ThrowingIngressReleasesBatch(){
    Fixture f;AerRealPresenter p;AerRealTick tick{-1,0,-1,5,{1,2,3}};auto selected=Batch(f,p,tick,{1,2});
    const auto before=p.PolicyState();std::array<AerRealCandidate,3> ready{Candidate(f,4),Candidate(f,3),Candidate(f,6)};bool threw=false;
    try{p.StepReady({++tick.frame,0,-1,5,{1,2,3}},ready,[](const AerReadyIngress&){throw std::runtime_error("injected ingress failure");});}
    catch(const std::runtime_error&){threw=true;}
    Check(threw&&p.PolicyState().sourceIds==before.sourceIds&&p.PolicyState().lastFrame==before.lastFrame,"throwing ingress committed a display admission");
    Check(f.Pins()==2,"throwing ingress retained unselected source owners");
    for(const auto& candidate:ready)Check(!candidate.frame.lease,"throwing ingress returned ownership to moved batch");
    selected=Batch(f,p,tick,{3,4});Check(selected.sourceIds==std::array<int64_t,2>{4,3},"presenter did not recover after observer exception");
    auto gpu=selected.OwnerForEye(0);selected={};std::array<AerRealCandidate,2> newScope{Candidate(f,5,7),Candidate(f,6,7)};threw=false;
    try{p.StepReady({++tick.frame,0,-1,5,{1,2,3}},newScope,[](const AerReadyIngress&){throw std::runtime_error("new scope ingress failure");});}
    catch(const std::runtime_error&){threw=true;}
    Check(threw&&p.HeldEyeCount()==0&&f.Pins()==1,"throw after preflight reset leaked batch or released external GPU source");
    p.Reset();gpu.reset();Check(f.Pins()==0,"throwing observer test leaked owners");
}
static void NoObserverAndNoopKeepDisplayBehavior(){
    Fixture f;AerRealPresenter optional,noOp;int64_t frame=-1;
    for(int64_t id:{1,2,4,5,4,6,7,8,8,9}){
        const AerRealTick tick{++frame,0,-1,5,{1,2,3}};
        auto a=optional.Step(tick,Candidate(f,id));std::array<AerRealCandidate,1> ready{Candidate(f,id)};
        auto b=noOp.StepReady(tick,ready,[](const AerReadyIngress&){});
        Check(a.sourceIds==b.sourceIds&&a.newlyAccepted==b.newlyAccepted&&a.candidateCode==b.candidateCode&&
              a.generation==b.generation&&a.sourceGeneration==b.sourceGeneration&&a.calibrationRevision==b.calibrationRevision&&
              a.referenceReceipt==b.referenceReceipt&&a.policyStep.delayForDraw==b.policyStep.delayForDraw&&
              a.policyStep.nextDelay==b.policyStep.nextDelay,"observer changed optional/default display behavior");
    }
    optional.Reset();noOp.Reset();Check(f.Pins()==0,"no-op observer retained batch");
}
int main(){try{
    OriginalQueueOrderAndRetainedBacklog();
    SkippedSourcesReleaseObsoleteFifoHeads();
    QueueScopeCapacityAndFailure();
    ProjectionOwnsNoGuideRecord();FourGuideRecordsRemainReusable();OriginalWindowsConsumeBothEyes();LateReadyLeftRegression();TransitionsBeforePolicy();InvalidBatchAndBound();
    OrderedBorrowedIngress();IngressAfterScopeAndCalibration();ThrowingIngressReleasesBatch();NoObserverAndNoopKeepDisplayBehavior();
    Check(PriorAerFixtureMain()==0,"optional Step compatibility regression");
    std::cout<<"PASS ready-batch production checks="<<checks<<" GPU=not-used\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}

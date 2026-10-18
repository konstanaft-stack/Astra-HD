#define main PriorNativeInputMain
#include "aer_native_input.cpp"
#undef main
#include <future>
#include <thread>

static void ConfigureCpuWorker(Context& c,cpvr::GenerationWorker& worker,NativeGenerationBridge& bridge){
    cpvr::GenerationPolicy policy;policy.generated_per_interval=0;
    policy.cadence=cpvr::Cadence::AlternateEyes;policy.maximum_eye_skew_ns=50'000'000;
    Check(cpvr::GenerationWorkerTestAccess::configure(worker,policy),"CPU policy setup failed");
    cpvr::GenerationWorkerTestAccess::enable_submission(worker,c.device.Get(),bridge.HistoryEpoch(),7,true);
}
static void FailedNewerDoesNotPoisonOrder(){
    Context c;cpvr::GenerationWorker worker;NativeGenerationBridge bridge(worker);ConfigureCpuWorker(c,worker,bridge);
    Capture newer(c,5,12),older(c,4,11);newer.frame.inputs[1].reset();Selection selected;
    Check(!bridge.SelectAlternating(c.device.Get(),c.queue.Get(),newer.frame,cpvr::Eye::Right,Target,selected),"missing guides input unexpectedly selected");
    bridge.SelectAlternating(c.device.Get(),c.queue.Get(),older.frame,cpvr::Eye::Left,Target,selected);
    Check(worker.status().accepted_inputs==1,"failed newer input poisoned order before successful submit");
}
static void MissingInputDoesNotHideReadyOutput(){
    Context c;cpvr::GenerationWorker worker;NativeGenerationBridge bridge(worker);ConfigureCpuWorker(c,worker,bridge);
    Capture left(c,0,7),right(c,1,8),missing(c,2,9);Selection selected;
    bridge.SelectAlternating(c.device.Get(),c.queue.Get(),left.frame,cpvr::Eye::Left,Target,selected);
    Check(bridge.SelectAlternating(c.device.Get(),c.queue.Get(),right.frame,cpvr::Eye::Right,Target,selected),"ready source pair unavailable");
    bridge.Submitted(selected,nullptr,0,false,false,false,0);selected={};missing.frame.inputs[0].reset();
    Check(bridge.SelectAlternating(c.device.Get(),c.queue.Get(),missing.frame,cpvr::Eye::Left,Target,selected),"missing new guides hid the existing ready output");
    Check(worker.status().accepted_inputs==2,"missing input was admitted");
    bridge.Submitted(selected,nullptr,0,false,false,false,0);
}
static NativeAlternatingOutputContext OutputContext(NativeGenerationBridge& bridge,const Capture& left,const Capture& right){
    NativeAlternatingOutputContext out;out.scope=left.frame.nativeStamp.eyes[0].mainEyeSource;
    out.resourceEpoch=left.frame.nativeStamp.resourceEpoch;out.captureClockGeneration=left.input->clockGeneration;out.historyEpoch=bridge.HistoryEpoch();
    out.expectedExtent={NativeAlternatingOutputContext::Extent{32,16},NativeAlternatingOutputContext::Extent{32,16}};
    out.maximumSourceId={left.frame.nativeStamp.eyes[0].mainEyeSource.sourceId,right.frame.nativeStamp.eyes[1].mainEyeSource.sourceId};return out;
}
static void SortedBatchAndLowerReference(){
    Context c;cpvr::GenerationWorker worker;NativeGenerationBridge bridge(worker);ConfigureCpuWorker(c,worker,bridge);
    Capture left(c,8,15),right(c,9,16);std::array<Capture*,2> batch{&right,&left};
    std::sort(batch.begin(),batch.end(),[](const Capture* a,const Capture* b){return a->frame.nativeStamp.eyes[a->eye].mainEyeSource.sourceId<b->frame.nativeStamp.eyes[b->eye].mainEyeSource.sourceId;});
    for(const auto* source:batch)Check(bool(bridge.IngestAlternating(c.device.Get(),c.queue.Get(),source->frame,cpvr::Eye(source->eye))),"sorted R/L batch dropped a real source");
    Check(worker.status().accepted_inputs==2,"batch did not enter single existing worker exactly once");
    const auto duplicate=bridge.IngestAlternating(c.device.Get(),c.queue.Get(),right.frame,cpvr::Eye::Right);
    Check(duplicate.status==NativeAlternatingIngestStatus::Duplicate&&worker.status().accepted_inputs==2,"immutable duplicate reentered worker");
    auto changed=right.frame;auto mismatched=std::make_shared<Inputs>(*right.input);++mismatched->poseWriteId;changed.inputs[1]=mismatched;
    Check(bridge.IngestAlternating(c.device.Get(),c.queue.Get(),changed,cpvr::Eye::Right).status==NativeAlternatingIngestStatus::Rejected,"duplicate ID bypassed actual full-guide identity validation");
    const auto old=bridge.IngestAlternating(c.device.Get(),c.queue.Get(),left.frame,cpvr::Eye::Left);
    Check(old.status==NativeAlternatingIngestStatus::Rejected,"late older source bypassed successful submission watermark");
    Selection output;auto context=OutputContext(bridge,left,right);
    Check(context.scope.sourceId==8&&bridge.SelectAlternatingReady(c.device.Get(),context,{1'120'000'000,11'111'111},output),"lower L reference incorrectly blocked selection after newer R ingestion");
    Check(output.native->selected.frame->eyes[0]->source_id==9&&output.native->selected.frame->eyes[1]->source_id==10,"output changed actual worker source namespace");
    bridge.Submitted(output,nullptr,0,false,false,false,0);
}
static void ConsumerCapacityDoesNotBlockIngestion(){
    Context c;cpvr::GenerationWorker worker;NativeGenerationBridge bridge(worker);ConfigureCpuWorker(c,worker,bridge);
    Capture left(c,0,7),right(c,1,8),next(c,2,9);
    Check(bool(bridge.IngestAlternating(c.device.Get(),c.queue.Get(),left.frame,cpvr::Eye::Left)),"left ingest failed");
    Check(bool(bridge.IngestAlternating(c.device.Get(),c.queue.Get(),right.frame,cpvr::Eye::Right)),"right ingest failed");
    auto context=OutputContext(bridge,left,right);std::vector<Selection> held;
    for(unsigned i=0;i<8;++i){Selection output;Check(bridge.SelectAlternatingReady(c.device.Get(),context,Target,output),"consumer capacity exhausted before eight leases");held.push_back(std::move(output));}
    Selection blocked;Check(!bridge.SelectAlternatingReady(c.device.Get(),context,Target,blocked)&&bridge.Status().code==NativeBridgeCode::ConsumerCapacity,"ninth output lease was allocated");
    const auto ingested=bridge.IngestAlternating(c.device.Get(),c.queue.Get(),next.frame,cpvr::Eye::Left);
    Check(bool(ingested)&&ingested.history==NativeAlternatingHistory::SameEyeDelta2&&worker.status().accepted_inputs==3,"output consumer slots blocked input/history admission");
    held.pop_back();bridge.Collect();context=OutputContext(bridge,next,right);
    Check(bridge.SelectAlternatingReady(c.device.Get(),context,Target,blocked),"released output slot did not become reusable");
    held.clear();bridge.Collect();ComPtr<CpuFence> fence;fence.Attach(new CpuFence(c.cpu.Get()));
    bridge.Submitted(blocked,fence.Get(),9,true,true,true,Target.target);blocked={};bridge.Collect();
    Check(bridge.Status().retainedConsumers==1,"GPU consumer retired before exact fence");
    bridge.Reset();Check(bridge.Status().retainedConsumers==1,"scope reset released in-flight consumer");
    fence->Signal(9);bridge.Collect();Check(bridge.Status().retainedConsumers==0,"completed consumer lease did not retire");
}
static void RetryKeepsHistoryAndOrder(){
    Context c;cpvr::GenerationWorker worker;NativeGenerationBridge bridge(worker);ConfigureCpuWorker(c,worker,bridge);
    Capture first(c,0,7),middle(c,1,8),newer(c,2,9);
    Check(bool(bridge.IngestAlternating(c.device.Get(),c.queue.Get(),first.frame,cpvr::Eye::Left)),"initial ingest failed");
    std::promise<void> acquired,release;auto wait=release.get_future();
    std::thread holder([&]{auto lock=cpvr::GenerationWorkerTestAccess::lock(worker);acquired.set_value();wait.wait();});
    acquired.get_future().wait();const auto retry=bridge.IngestAlternating(c.device.Get(),c.queue.Get(),newer.frame,cpvr::Eye::Left);
    release.set_value();holder.join();
    Check(retry.status==NativeAlternatingIngestStatus::Retry&&retry.code==NativeBridgeCode::InputRetry&&worker.status().accepted_inputs==1,"failed try-lock was reported as source admission");
    const auto m=bridge.IngestAlternating(c.device.Get(),c.queue.Get(),middle.frame,cpvr::Eye::Right);
    const auto n=bridge.IngestAlternating(c.device.Get(),c.queue.Get(),newer.frame,cpvr::Eye::Left);
    Check(bool(m)&&m.history==NativeAlternatingHistory::AdjacentNative&&bool(n)&&n.history==NativeAlternatingHistory::SameEyeDelta2,"retry advanced order or replaced proven middle history");
}
static void GapThenConsecutiveSourcesRecover(){
    Context c;cpvr::GenerationWorker worker;NativeGenerationBridge bridge(worker);ConfigureCpuWorker(c,worker,bridge);
    const std::array<int64_t,6> ids{0,1,3,4,5,6};
    const std::array<NativeAlternatingHistory,6> histories{NativeAlternatingHistory::Warmup,NativeAlternatingHistory::AdjacentNative,
        NativeAlternatingHistory::Warmup,NativeAlternatingHistory::AdjacentNative,NativeAlternatingHistory::SameEyeDelta2,NativeAlternatingHistory::SameEyeDelta2};
    for(size_t i=0;i<ids.size();++i){Capture source(c,ids[i],uint32_t(7+ids[i]));
        const auto result=bridge.IngestAlternating(c.device.Get(),c.queue.Get(),source.frame,cpvr::Eye(source.eye));
        Check(bool(result)&&result.history==histories[i],"missing source caused permanent rejection or invented a temporal span");}
    Check(worker.status().accepted_inputs==ids.size(),"gap dropped otherwise valid real inputs");
}
static void BatchRetryCannotBeOvertaken(){
    Context c;cpvr::GenerationWorker worker;NativeGenerationBridge bridge(worker);ConfigureCpuWorker(c,worker,bridge);
    Capture left(c,0,7),right(c,1,8);const std::array<Capture*,2> batch{&left,&right};
    std::promise<void> acquired,release;auto wait=release.get_future();
    std::thread holder([&]{auto lock=cpvr::GenerationWorkerTestAccess::lock(worker);acquired.set_value();wait.wait();});
    acquired.get_future().wait();unsigned visited=0;NativeAlternatingIngestResult first;
    IngestAlternatingBatch(batch,[&](const auto* source){
        const auto result=bridge.IngestAlternating(c.device.Get(),c.queue.Get(),source->frame,cpvr::Eye(source->eye));
        if(++visited==1){first=result;release.set_value();holder.join();}
        return result;
    });
    Check(first.status==NativeAlternatingIngestStatus::Retry,"CPU contention did not exercise Retry");
    Check(visited==1&&worker.status().accepted_inputs==0,"newer source overtook retry and closed the earlier admission window");
    IngestAlternatingBatch(batch,[&](const auto* source){
        auto result=bridge.IngestAlternating(c.device.Get(),c.queue.Get(),source->frame,cpvr::Eye(source->eye));
        Check(bool(result),"ordered retry batch could not recover both source eyes");return result;
    });
    Check(worker.status().accepted_inputs==2,"retry recovery did not admit both sources exactly once");
}
static void BatchRejectedAndDuplicateDoNotStop(){
    Context c;cpvr::GenerationWorker worker;NativeGenerationBridge bridge(worker);ConfigureCpuWorker(c,worker,bridge);
    Capture invalid(c,0,7),right(c,1,8),left(c,2,9);invalid.frame.inputs[0].reset();
    const std::array<Capture*,4> batch{&invalid,&right,&right,&left};
    std::vector<NativeAlternatingIngestStatus> observed;
    IngestAlternatingBatch(batch,[&](const auto* source){
        auto result=bridge.IngestAlternating(c.device.Get(),c.queue.Get(),source->frame,cpvr::Eye(source->eye));
        observed.push_back(result.status);return result;
    });
    const std::vector<NativeAlternatingIngestStatus> expected{NativeAlternatingIngestStatus::Rejected,NativeAlternatingIngestStatus::Submitted,
        NativeAlternatingIngestStatus::Duplicate,NativeAlternatingIngestStatus::Submitted};
    Check(observed==expected&&worker.status().accepted_inputs==2,"terminal reject/duplicate prevented a later valid source");
}
static void ExplicitOutputScopeAndBounds(){
    Context c;cpvr::GenerationWorker worker;NativeGenerationBridge bridge(worker);ConfigureCpuWorker(c,worker,bridge);
    Capture left(c,2,9),right(c,3,10);bridge.IngestAlternating(c.device.Get(),c.queue.Get(),left.frame,cpvr::Eye::Left);
    bridge.IngestAlternating(c.device.Get(),c.queue.Get(),right.frame,cpvr::Eye::Right);const auto current=OutputContext(bridge,left,right);
    for(unsigned mutation=0;mutation<8;++mutation){auto bad=current;Selection output;
        switch(mutation){case 0:++bad.scope.epochs.gameWorld;break;case 1:++bad.scope.latchRevision;break;case 2:++bad.historyEpoch;break;
            case 3:++bad.resourceEpoch;break;case 4:++bad.captureClockGeneration;break;case 5:++bad.expectedExtent[1].width;break;
            case 6:bad.maximumSourceId={0,1};break;case 7:++bad.scope.clockGeneration;break;}
        Check(!bridge.SelectAlternatingReady(c.device.Get(),bad,{1'080'000'000,11'111'111},output)&&!output.native,"invalid explicit baseline produced a consumer lease");}
    Selection output;Check(bridge.SelectAlternatingReady(c.device.Get(),current,{1'080'000'000,11'111'111},output),"output guards poisoned valid selection");
    bridge.Submitted(output,nullptr,0,true,false,false,0);output={};bridge.Reset();bridge.Collect();
    Check(bridge.Status().retainedConsumers==1,"unknown GPU completion was released by split/reset");
}
static void EpochRetryAndRejectedInputRelease(){
    Context c;cpvr::GenerationWorker worker;NativeGenerationBridge bridge(worker);ConfigureCpuWorker(c,worker,bridge);
    Capture old(c,0,7),fresh(c,2,9);Check(bool(bridge.IngestAlternating(c.device.Get(),c.queue.Get(),old.frame,cpvr::Eye::Left)),"old scope ingest failed");
    const auto oldEpoch=bridge.HistoryEpoch();fresh.frame.nativeStamp.eyes[0].mainEyeSource.epochs.gameWorld++;
    Check(bridge.IngestAlternating(c.device.Get(),c.queue.Get(),fresh.frame,cpvr::Eye::Left).status==NativeAlternatingIngestStatus::Retry&&bridge.HistoryEpoch()!=oldEpoch,"new scope reused old history");
    Check(cpvr::GenerationWorkerTestAccess::apply_history_reset(worker),"CPU owner did not complete reset");
    Check(bool(bridge.IngestAlternating(c.device.Get(),c.queue.Get(),fresh.frame,cpvr::Eye::Left)),"new scope did not bootstrap after reset");
    const auto epoch=bridge.HistoryEpoch();Check(bridge.IngestAlternating(c.device.Get(),c.queue.Get(),old.frame,cpvr::Eye::Left).status==NativeAlternatingIngestStatus::Rejected&&bridge.HistoryEpoch()==epoch,"stale scope reset the current bridge");
    Capture invalid(c,4,11);invalid.frame.nativeStamp.eyes[0].mainEyeSource.epochs.gameWorld++;
    invalid.input->poseWriteId++;std::weak_ptr<const Inputs> guides=invalid.input;std::weak_ptr<ReadLease> color=invalid.frame.lease;
    Check(bridge.IngestAlternating(c.device.Get(),c.queue.Get(),invalid.frame,cpvr::Eye::Left).status==NativeAlternatingIngestStatus::Rejected,"mismatched full guides admitted");
    invalid.frame={};invalid.input.reset();Check(guides.expired()&&color.expired(),"rejected input left a bridge-owned pending guide/color pin");
}
int main(){
    unsigned failures=0;
    const std::pair<const char*,void(*)()> cases[]={{"failed_newer_order",FailedNewerDoesNotPoisonOrder},{"output_without_new_guides",MissingInputDoesNotHideReadyOutput},
        {"sorted_batch_lower_reference",SortedBatchAndLowerReference},{"consumer_capacity",ConsumerCapacityDoesNotBlockIngestion},
        {"retry_order_history",RetryKeepsHistoryAndOrder},{"gap_recovery",GapThenConsecutiveSourcesRecover},
        {"batch_retry_order",BatchRetryCannotBeOvertaken},{"batch_terminal_continue",BatchRejectedAndDuplicateDoNotStop},
        {"explicit_output_context",ExplicitOutputScopeAndBounds},{"epoch_retry_ownership",EpochRetryAndRejectedInputRelease},
        {"prior_cpu_fixture",[]{char name[]="prior";char* argv[]{name};Check(PriorNativeInputMain(1,argv)==0,"prior CPU adapter/bridge guards regressed");}}};
    for(const auto& [name,test]:cases)try{test();std::cout<<"PASS "<<name<<'\n';}
        catch(const std::exception& e){++failures;std::cerr<<"FAIL "<<name<<": "<<e.what()<<'\n';}
    std::cout<<"RESULT cases="<<std::size(cases)<<" failures="<<failures<<" checks="<<checks<<" GPU=not-used\n";
    return failures?1:0;
}

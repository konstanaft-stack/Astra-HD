#include "Framegen/ProducerTrace.hpp"
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

using namespace cvr::framegen;
static void Check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
static NodePoseEvidence Pose() {return {31,41,7,1,99,true,true};}
static void Scope() {
    const auto outer=EnterProducerNodeScope(true,0,0,0x1234,0x456);
    Check(CurrentProducerNodeScope().active && CurrentProducerNodeScope().depth==1,"node entry was not stamped");
    const auto parent=EnterProducerNodeScope(true,777,1,0x5678,0x789);
    Check(CurrentProducerNodeScope().engineView==EngineView::Vrcam && CurrentProducerNodeScope().depth==2,"nested node overwrote its parent evidence");
    bool separate=false;std::thread other([&]{separate=!CurrentProducerNodeScope().active;});other.join();
    Check(separate,"node scope leaked into another thread");
    RestoreProducerNodeScope(parent);
    Check(CurrentProducerNodeScope().engineView==EngineView::Main && CurrentProducerNodeScope().context==0x1234,"nested exit failed to restore parent");
    RestoreProducerNodeScope(outer);Check(!CurrentProducerNodeScope().active,"node exit retained active scope");
}
static void Identity() {
    const auto missing=DescribeProducerNode({},{});
    Check(missing.reason==ProducerReason::NoNodeScope,"missing scope conflated with invalid identity");
    ProducerNodeScope node{true,true,EngineView::Main,0,0x1234,1,0x456};
    const auto invalid=DescribeProducerNode(node,{});
    Check(invalid.reason==ProducerReason::InvalidPoseInScope,"in-scope invalid identity not distinguished");
    auto pose=Pose();pose.view=9;
    Check(DescribeProducerNode(node,pose).reason==ProducerReason::PoseViewUnsupported,"unsupported pose view accepted");
    pose=Pose();pose.hasNativeFrame=false;
    Check(DescribeProducerNode(node,pose).reason==ProducerReason::NativeFrameMissing,"missing native frame guessed");
    pose=Pose();pose.headValid=false;
    Check(DescribeProducerNode(node,pose).reason==ProducerReason::InvalidPoseInScope,"invalid head admitted by nonzero pose IDs");
    // Scope markers are observational: no new rejection is added to the old
    // valid-pose gate solely because scope evidence is absent.
    Check(DescribeProducerNode({},Pose()).reason==ProducerReason::TagsAccepted,"diagnostic changed the author's valid-pose gate");
}
static void Provenance() {
    ProducerNodeScope node{true,true,EngineView::Vrcam,777,0x1234,2,0x456};
    const auto record=DescribeProducerNode(node,Pose());
    Check(record.stage==ProducerStage::TagAdmission,"producer event claimed a draw/submit stage");
    Check(record.engineView==EngineView::Vrcam && record.identityView==1,"actual engine view replaced by pose view");
    Check(record.nativeFrame==99 && record.poseId==31 && record.poseWriteId==41 && record.spaceEpoch==7,"actual frame/pose identifiers lost");
    Check(record.flags&NativeFrameKnown,"known renderer frame was discarded");
    Check(!(record.flags&LogicalEyeKnown),"MAIN/VRCAM guessed into left/right");
    Check(!(record.flags&(SourceIdKnown|PreviousSourceIdKnown|PreviousNativeFrameKnown|PoseTimeValid|ContentTimeValid|ResourceEpochKnown|
        RequestedCycleModeKnown|ActiveCycleModeKnown)),"unknown provenance or active AER mode fabricated");
    Check(record.sourceId==0 && record.previousSourceId==0 && record.poseTimeNs==0 && record.contentTimeNs==0,"missing time/source gained a fallback");
}
static void Ring() {
    ProducerTraceStorage trace;
    for(uint64_t source=1;source<=80;++source) {
        ProducerTraceRecord record;record.reason=ProducerReason::ColorScheduled;
        record.sourceId=source;record.flags=SourceIdKnown|CaptureSubmissionQpcValid;
        record.captureSubmissionQpc=1000+source;
        Check(trace.Record(record),"uncontended trace write dropped");
    }
    Check(trace.published.load()==80 && trace.dropped.load()==0,"trace accounting lost writes");
    Check(trace.slots[0].sequence==65 && trace.slots[0].record.sourceId==65,"bounded ring failed to wrap");
    const auto& last=trace.slots[15];
    Check(last.sequence==80 && last.record.captureSubmissionQpc==1080,"latest event mismatched its sequence");
    Check(!(last.record.flags&(PoseTimeValid|ContentTimeValid)) && last.record.contentTimeNs==0,"capture QPC was relabeled content time");
    Check(trace.counters[unsigned(ProducerReason::ColorScheduled)].load()==80,"reason counter depends on DEBUG");
    Check(!(trace.writeEpoch.load()&1),"finished trace remains externally unreadable");
}
static void Contention() {
    ProducerTraceStorage trace;trace.writeLock.store(1);
    ProducerTraceRecord record;record.reason=ProducerReason::NoNodeScope;
    Check(!trace.Record(record),"trace wrote through an occupied writer lock");
    Check(trace.attempted.load()==1 && trace.dropped.load()==1 && trace.published.load()==0,"dropped diagnostic fabricated a published record");
    Check(trace.counters[unsigned(ProducerReason::NoNodeScope)].load()==1,"contention erased the rejection counter");
    trace.writeLock.store(0);Check(trace.Record(record),"trace did not recover after contention");
    Check(trace.slots[0].sequence==1 && trace.slots[0].record.reason==ProducerReason::NoNodeScope,"recovered trace has false provenance");
}
int main(int argc,char** argv) {
    try {
        const std::string_view name=argc>1?argv[1]:"";
        if(name=="scope")Scope();else if(name=="identity")Identity();else if(name=="provenance")Provenance();
        else if(name=="ring")Ring();else if(name=="contention")Contention();else throw std::runtime_error("unknown trace case");
        std::cout << "PASS " << name << '\n';return 0;
    } catch(const std::exception& e) {std::cerr << "FAIL " << e.what() << '\n';return 1;}
}

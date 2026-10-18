#include "Framegen/OwnSelectionWitness.hpp"
#include "Framegen/FramePolicy.hpp"
#include <algorithm>
#include <memory>
#include <vector>
#include <cassert>
#include <cstdio>
struct ID3D12Device{};struct ID3D12CommandQueue{};struct ID3D12Fence{bool same=true;};
namespace cvr::framegen {
bool TestBridgeFovDecision(OwnSelectionWitness&,uint64_t,uint64_t,float);
struct PresentationTime{int64_t target{},period{};};
struct ReadLease{uint64_t fence{};};struct Frame{Identity identity;std::shared_ptr<ReadLease> lease;};
struct Pair{uint64_t pair_id=20,epoch=1,space_epoch=2;};
struct Device{ID3D12Device* Get()const{return nullptr;}};
struct NativeSelectionLease{struct{std::shared_ptr<Pair> frame;}selected;Device device;ID3D12Fence* consumerFence{};uint64_t consumerValue{};bool submissionReported{},copySubmitted{},signalSucceeded{};};
struct Selection{Frame frame;Cadence::Next kind=Cadence::Next::Real;std::shared_ptr<ReadLease> generatedFrom;bool synthetic{};std::shared_ptr<NativeSelectionLease> native;OwnSelectionWitness ownWitness;};
enum class NativeBridgeCode{Ready,IncompleteCamera,MissingCameraTime,MissingPoseTime,BackendPending,ConsumerCompletionUnknown};
struct Worker{unsigned acknowledgements{},committed{};bool ackReturn{};bool acknowledge(uint64_t pair,uint64_t epoch,uint64_t space,int64_t time){assert(pair==20&&epoch==1&&space==2&&time>0);++acknowledgements;if(ackReturn)++committed;return ackReturn;}int status(){return 0;}};
inline OwnWitnessCounters WitnessCounters(int,int64_t){return {};}
inline bool SameDevice(ID3D12Fence* fence,ID3D12Device*){return fence->same;}
class NativeGenerationBridge {
public:
 struct Impl{std::vector<std::shared_ptr<NativeSelectionLease>> consumers;Worker* worker;uint64_t historyEpoch=1;struct{NativeBridgeCode code=NativeBridgeCode::Ready;}status;} storage;
 Impl* impl=&storage;OwnWitnessRing ring;bool ready{};uint64_t attempt{};
 NativeGenerationBridge(Worker& worker){storage.worker=&worker;}
 bool Select(ID3D12Device*,ID3D12CommandQueue*,const Frame& current,PresentationTime time,Selection& out){
  out.ownWitness={};auto& w=out.ownWitness;w.attempt=++attempt;w.pair=20;w.candidateKnown=true;w.target=time.target;w.sourceSerial=current.identity.serial;w.history=w.candidateHistory=1;w.space=w.candidateSpace=2;
  if(!ready)return TestBridgeFovDecision(w,current.identity.serial,1,.00003f);
  w.bridge=OwnBridgeReason::Ready;
  ResetSelectionPreservingOwnWitness(out);out.frame=current;out.frame.identity.serial=99;
  out.native=std::make_shared<NativeSelectionLease>();out.native->selected.frame=std::make_shared<Pair>();storage.consumers.push_back(out.native);return true;
 }
 void Submitted(const Selection&,ID3D12Fence*,uint64_t,bool,bool,bool,int64_t);
 void ObserveOwnFallback(const Selection&,bool,bool,bool,int64_t)noexcept;
 void RecordOwnWitness(OwnSelectionWitness& w)noexcept{ring.Store(w);}
 void Collect(){}void Reset(){}auto Status()const{return storage.status;}
};
NativeGenerationBridge* nativeBridge;Cadence cadence;Frame pending;
struct{void ConsumerFence(ID3D12Fence*,uint64_t){}}generator;
inline auto GetSettings(){struct Settings{bool enabled=true;};return Settings{};}
inline void PacingReady(bool){}inline bool ExternalBridgeLoaded(){return false;}inline void Status(const char*,bool=false){}
inline bool MetricsEnabled(){return false;}inline void OnSubmitted(uint64_t,bool,bool,double){}inline double NowMs(){return 0;}inline void Reset(const char*){}
}
#include "OwnProductionEndpoints.hpp"
using namespace cvr::framegen;
int main(){
 Worker worker;NativeGenerationBridge bridge(worker);nativeBridge=&bridge;Frame current;current.identity.serial=7;current.lease=std::make_shared<ReadLease>();
 auto fallback=TestPresenterSelect(nullptr,nullptr,current,true,{100,10});
 assert(fallback.frame.identity.serial==7&&fallback.frame.lease==current.lease&&!fallback.native);
 assert(fallback.ownWitness.presenter==OwnPresenterPath::CurrentRealFallback);
 assert(fallback.ownWitness.bridge==OwnBridgeReason::RenderedFovDelta&&fallback.ownWitness.bridgeEye==1&&fallback.ownWitness.admittedSerial==7&&fallback.ownWitness.history==1);
 ID3D12Fence fence;TestPresenterSubmitted(fallback,&fence,1,true,true,true,101);
 assert(bridge.ring.Find(1)->accepted&&!bridge.ring.Find(1)->ackCall&&worker.acknowledgements==0);
 bridge.ready=true;auto rejected=TestPresenterSelect(nullptr,nullptr,current,true,{110,10});
 assert(rejected.native&&rejected.ownWitness.presenter==OwnPresenterPath::NativeSelected);rejected.ownWitness.downstream=OwnDownstreamReason::SelectedNativeGeometryRejected;
 TestPresenterSubmitted(rejected,&fence,0,false,false,false,111);assert(!bridge.ring.Find(2)->ackCall&&worker.acknowledgements==0);
 current.identity.serial=8;auto retry=TestPresenterSelect(nullptr,nullptr,current,true,{120,10});
 assert(retry.ownWitness.pair==rejected.ownWitness.pair&&retry.ownWitness.sourceSerial==8&&retry.ownWitness.samePairRun==3);
 fence.same=false;TestPresenterSubmitted(retry,&fence,2,true,true,true,121);
 assert(!bridge.ring.Find(3)->signalSucceeded&&!bridge.ring.Find(3)->ackCall&&worker.acknowledgements==0);
 fence.same=true;auto accepted=TestPresenterSelect(nullptr,nullptr,current,true,{130,10});TestPresenterSubmitted(accepted,&fence,3,true,true,true,131);
 assert(worker.acknowledgements==1&&worker.committed==0&&bridge.ring.Find(4)->AckMayBeDeferred());
 worker.ackReturn=true;auto committed=TestPresenterSelect(nullptr,nullptr,current,true,{140,10});TestPresenterSubmitted(committed,&fence,4,true,true,true,141);
 assert(worker.acknowledgements==2&&worker.committed==1&&bridge.ring.Find(5)->ackReturn);
 std::puts("PASS actual Presenter native/fallback and Bridge Submitted source fragments: veto/retry, validated fence, ACK call/return join");
}

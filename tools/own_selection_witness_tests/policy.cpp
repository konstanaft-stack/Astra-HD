#include "Framegen/OwnSelectionWitness.hpp"
#include <cassert>
#include <type_traits>
#include <cstdio>
using namespace cvr::framegen;
int main(){
 static_assert(std::is_trivially_copyable_v<OwnSelectionWitness>);
 struct Selection {uint64_t current=10;OwnSelectionWitness ownWitness;};
 Selection fallback;auto& w=fallback.ownWitness;w.attempt=1;w.pair=20;w.candidateKnown=true;w.target=100;
 w.bridge=OwnBridgeReason::RenderedFovDelta;w.presenter=OwnPresenterPath::CurrentRealFallback;
 assert(fallback.current==10);OwnWitnessRing ring;ring.Store(w);
 auto submit=w;OwnWitnessSubmission(submit,true,true,true,101,false,false,false);ring.Store(submit);
 assert(ring.Find(1)->accepted&&!ring.Find(1)->ackCall&&fallback.current==10);
 Selection selected;selected.ownWitness=w;selected.ownWitness.attempt=2;selected.ownWitness.bridge=OwnBridgeReason::Ready;
 ResetSelectionPreservingOwnWitness(selected);selected.current=30;selected.ownWitness.presenter=OwnPresenterPath::NativeSelected;
 selected.ownWitness.downstream=OwnDownstreamReason::SelectedNativeGeometryRejected;
 OwnWitnessSubmission(selected.ownWitness,false,false,false,102,false,false,false);ring.Store(selected.ownWitness);
 assert(!ring.Find(2)->accepted&&!ring.Find(2)->ackCall&&ring.Find(2)->pair==20);
 auto retry=selected.ownWitness;retry.attempt=3;retry.target=110;retry.submissionKnown=false;ring.Store(retry);
 assert(retry.samePairRun==3&&retry.samePairFirstTarget==100);
 assert(!retry.ackCall);auto accepted=retry;accepted.attempt=4;accepted.target=120;accepted.downstream=OwnDownstreamReason::GeometryReady;
 OwnWitnessSubmission(accepted,true,true,true,121,true,true,false);ring.Store(accepted);
 assert(ring.Find(4)->AckMayBeDeferred()&&ring.Find(4)->ackCall);
 accepted.attempt=5;OwnWitnessSubmission(accepted,true,true,true,122,true,true,true);ring.Store(accepted);
 assert(!ring.Find(5)->AckMayBeDeferred());
 OwnWitnessGeometry g;g.Compare();assert(!g.comparisonKnown);g.previousResolved=g.currentResolved=true;
 g.previousFov={1,2,3,4};g.currentFov={1,2,3.00003f,4};g.Compare();assert(g.comparisonKnown&&g.maximumDelta>.00002f);
 for(uint64_t i=6;i<100;++i){auto n=w;n.attempt=i;n.pair=i;n.submissionKnown=true;ring.Store(n);}
 assert(ring.Count()==64&&ring.Evicted()==35&&!ring.Find(1));
 unsigned logs{};for(uint64_t i=100;i<1100;++i){auto n=w;n.attempt=i;n.submissionKnown=true;ring.Store(n);if(ring.LogDue(n,1000))++logs;}
 assert(logs==8);auto n=w;n.attempt=1100;n.submissionKnown=true;assert(ring.LogDue(n,2000));assert(!ring.LogDue(n,2000));
 std::puts("PASS scalar witness models, fallback reset preservation, deferred-ACK interpretation and bounded ring/logging (not production endpoint execution)");
}

#include "Runtimes/MainCaptureCatalogue.hpp"
#include "Runtimes/CapturePoolPolicy.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace cvr::capture;
using namespace cvr::framegen;
static void Require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
static MainEyeSourceReceipt Receipt(int64_t id){return {1,uint64_t(id+1),2,{20,10,30},id,AerV2Eye(id&1),40,5000+id,false};}
struct Metadata {uint64_t fence{};};
struct PendingCopy {int64_t at{};uint64_t fence{};uintptr_t resource{};};
struct GpuRead {int64_t at{};uintptr_t resource{};};
struct Result {
    unsigned pool{},phase{},readLag{},produced{},noSlot{},fresh[2]{},duplicates{},maxCaptureInFlight{},maxDistinctReaders{},maxExtraReaders{};
    unsigned maxSimultaneousOwned{},overwritePending{},overwriteReady{};
};
static Result Replay(unsigned count,unsigned phase,unsigned readLag=0){
    constexpr int64_t ProducerUs=14000,ConsumerUs=11111,DurationUs=10000000;
    constexpr int64_t CaptureLatencyUs=3*ProducerUs;
    MainCaptureCatalogue<Metadata,8> catalogue;BufferLeases leases;
    std::array<uintptr_t,8> pool{};for(unsigned i=0;i<count;++i)pool[i]=i+1;
    catalogue.Reset(20,std::span(pool).first(count));
    std::array<uintptr_t,2> held{pool[0],pool[1]};std::array<int64_t,2> heldId{0,1};
    for(unsigned i=0;i<2;++i){const auto key=catalogue.BeginWrite(i,catalogue.Generation());
        Require(catalogue.Publish(key,Receipt(i),i+1,Metadata{i+1}),"seed publish failed");Require(leases.Pin(pool[i]),"seed pin failed");}
    uint64_t fence=2,completed=2;uint32_t lastSlot=1;uintptr_t published=pool[1];int64_t sourceId=2;
    std::vector<PendingCopy> copies;std::vector<GpuRead> reads;
    int64_t nextProducer=0,nextConsumer=phase;Result out;out.pool=count;out.phase=phase;out.readLag=readLag;
    while(std::min(nextProducer,nextConsumer)<DurationUs){
        const auto now=std::min(nextProducer,nextConsumer);
        copies.erase(std::remove_if(copies.begin(),copies.end(),[&](const auto& point){if(point.at>now)return false;completed=std::max(completed,point.fence);return true;}),copies.end());
        reads.erase(std::remove_if(reads.begin(),reads.end(),[&](const auto& point){if(point.at>now)return false;leases.Unpin(point.resource);return true;}),reads.end());
        if(now==nextProducer){
            uint64_t owners{};for(auto resource:held)owners|=uint64_t{1}<<(resource-1);
            for(const auto& read:reads)owners|=uint64_t{1}<<(read.resource-1);
            for(const auto& copy:copies)owners|=uint64_t{1}<<(copy.resource-1);
            out.maxSimultaneousOwned=std::max(out.maxSimultaneousOwned,unsigned(std::popcount(owners)));
            // Exact production order: Reserve/BeginWrite and metadata invalidate
            // occur BEFORE the three-allocator fenceValue-2 reuse gate.
            const auto slot=ReserveMainWrite(std::span(pool).first(count),published,lastSlot,leases);
            if(!slot)++out.noSlot;
            else {
                const auto old=catalogue.ReadyKeys(Receipt(sourceId),[](const auto&){return true;});
                for(size_t i=0;i<old.size;++i)if(old.keys[i].slot==*slot){const auto* entry=catalogue.Find(old.keys[i]);
                    if(entry->receipt.sourceId>heldId[unsigned(entry->receipt.physicalEye)]){
                        if(entry->payload.fence>completed)++out.overwritePending;else ++out.overwriteReady;
                    }
                }
                const auto ticket=catalogue.BeginWrite(*slot,catalogue.Generation());
                Require(fence<3 || completed>=fence-2,"replay exceeded actual three-context admission bound");
                ++fence;lastSlot=*slot;published=pool[*slot];
                Require(catalogue.Publish(ticket,Receipt(sourceId),uint64_t(sourceId+1),Metadata{fence}),"copy publication failed");
                leases.EndWrite(published);copies.push_back({now+CaptureLatencyUs,fence,published});++out.produced;
                out.maxCaptureInFlight=std::max(out.maxCaptureInFlight,unsigned(copies.size()));
                Require(copies.size()<=3,"more than three captures in flight");
            }
            ++sourceId;nextProducer+=ProducerUs;
        }
        if(now==nextConsumer){
            // Optimistic consumer: every ready newer same-eye source is eligible.
            // If it starves here, a stricter AER policy cannot manufacture a frame.
            auto keys=catalogue.ReadyKeys(Receipt(sourceId),[&](const auto& entry){return entry.payload.fence<=completed&&!leases.Writing(entry.key.resource);});
            std::array<bool,8> keep{};std::array<int,2> winner{-1,-1};
            for(size_t i=0;i<keys.size;++i){const auto* entry=catalogue.Find(keys.keys[i]);Require(entry&&leases.Pin(entry->key.resource),"ready pin failed");
                const unsigned eye=unsigned(entry->receipt.physicalEye);
                if(winner[eye]<0 && entry->receipt.sourceId>heldId[eye])winner[eye]=int(i);
            }
            for(unsigned eye=0;eye<2;++eye)if(winner[eye]>=0){
                const auto i=size_t(winner[eye]);const auto* entry=catalogue.Find(keys.keys[i]);
                leases.Unpin(held[eye]);held[eye]=entry->key.resource;heldId[eye]=entry->receipt.sourceId;keep[i]=true;++out.fresh[eye];
            }
            if(winner[0]<0&&winner[1]<0)++out.duplicates;
            for(size_t i=0;i<keys.size;++i)if(!keep[i])leases.Unpin(keys.keys[i].resource);
            // Optional independent, bounded reconstruction ticket owners. Same
            // resources as held eyes count once in the DISTINCT resource bound.
            if(readLag && reads.size()+2<=6)for(unsigned eye=0;eye<2;++eye){Require(leases.Pin(held[eye]),"GPU owner pin failed");reads.push_back({now+readLag,held[eye]});}
            unsigned distinct{},extra{};for(unsigned i=0;i<count;++i)if(leases.Readers(pool[i])){++distinct;if(pool[i]!=held[0]&&pool[i]!=held[1])++extra;}
            out.maxDistinctReaders=std::max(out.maxDistinctReaders,distinct);out.maxExtraReaders=std::max(out.maxExtraReaders,extra);
            nextConsumer+=ConsumerUs;
        }
    }
    for(const auto& r:reads)leases.Unpin(r.resource);for(auto resource:held)leases.Unpin(resource);
    for(unsigned i=0;i<count;++i)Require(leases.Readers(pool[i])==0&&!leases.Writing(pool[i]),"replay leaked ownership");
    return out;
}
int main(){try{
    std::cout<<"model_us producer=14000 consumer=11111 capture_latency=42000 duration=10000000; GPU_execution=0\n";
    std::cout<<"pool,phase_us,gpu_read_lag_us,produced,freshL,freshR,noSlot,duplicatePolls,maxCaptureInFlight,maxDistinctReaderCells,maxExtraReaderCells,maxSimultaneousOwnedBeforeReserve,overwritePendingUnconsumed,overwriteReadyUnconsumed\n";
    auto print=[](const Result&r){std::cout<<r.pool<<','<<r.phase<<','<<r.readLag<<','<<r.produced<<','<<r.fresh[0]<<','<<r.fresh[1]<<','<<r.noSlot<<','<<r.duplicates<<','<<r.maxCaptureInFlight<<','<<r.maxDistinctReaders<<','<<r.maxExtraReaders<<','<<r.maxSimultaneousOwned<<','<<r.overwritePending<<','<<r.overwriteReady<<'\n';};
    for(unsigned phase:{0u,1000u,5000u,10000u})for(unsigned count:{4u,5u,6u}){
        const auto r=Replay(count,phase);print(r);
        if(count<6)Require(r.fresh[0]==0&&r.fresh[1]==0,"expected hot-cell overwrite counterexample disappeared");
        else Require(r.fresh[0]>300&&r.fresh[1]>300,"six cells did not recover both eyes under three-context lag");
    }
    // Same queue ordering: both capture and app-read commands complete at the
    // same fixed 42 ms lag. Six reconstruction slots limit outstanding reads.
    for(unsigned count:{6u,7u,8u}){
        const auto pressure=Replay(count,5000,42000);print(pressure);
        Require(pressure.maxExtraReaders>0&&pressure.maxCaptureInFlight<=3,"distinct GPU-reader pressure case absent");
    }
    std::cout<<"PASS actual ReserveMainWrite+MainCaptureCatalogue: 4/5 starve, 6 sustains ready opportunities with two persistent readers; extra distinct GPU owners are reported separately\n";
    return 0;
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}

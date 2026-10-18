#include <array>
#include <atomic>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include "Runtimes/CaptureBufferLeases.hpp"
#include "Runtimes/CapturePoolPolicy.hpp"
#include "Framegen/Presenter.hpp"
#include "CpuD3d.hpp"
#if __has_include("Runtimes/MainCaptureCatalogue.hpp")
#include "Runtimes/MainCaptureCatalogue.hpp"
#define HAVE_CATALOGUE 1
#endif
using namespace cvr::framegen;
using Microsoft::WRL::ComPtr;
static unsigned checks{};
static void Check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
static MainEyeSourceReceipt Scope(int64_t id=0){
    return {1,uint64_t(id+1),2,{3,4,5},id,AerV2Eye(id&1),6,1000+id,false};
}
struct OpenXRManager {
    std::mutex m_presentMutex;
    cvr::capture::BufferLeases m_captureLeases;
    std::array<ID3D12Resource*,8> m_monoPool{};
    uint32_t m_monoPoolCount=4;uint64_t m_captureResourceEpoch=3;
    std::atomic<bool> m_monoSubmitEnabled{true};
    struct Latest {double captureMs{};std::shared_ptr<const Inputs> framegenInputs[2];} m_monoCapturedFrame;
    unsigned releases{};
#if HAVE_CATALOGUE
    cvr::capture::MainCaptureCatalogue<Frame,8> m_mainCaptureCatalogue;
#else
    Frame latest;
#endif
    std::vector<Frame> CollectReadyMainCaptures(const MainEyeSourceReceipt&);
#if HAVE_CATALOGUE
    void PublishExact(const decltype(m_mainCaptureCatalogue)::Key& catalogueWrite,Frame value,
                      const std::shared_ptr<const Inputs>& guides,bool publishMainSourceOnly=true) {
        const uint32_t publishedMainEye=value.color[0]?0u:1u;const auto eye=publishedMainEye;
        auto* snapshot=value.color[eye];const auto serial=value.identity.serial;
        const auto nativeStamp=value.nativeStamp;const auto captureCompletion=value.colorCompletion;
        XrPosef capturedPoses[2]{value.pose[0],value.pose[1]};XrFovf fovs[2]{value.fov[0],value.fov[1]};
        uint64_t capturedPoseIds[2]{value.identity.pose[0],value.identity.pose[1]},capturedImageGenerations[2]{71,72};
        std::shared_ptr<const Inputs> capturedInputs[2];capturedInputs[eye]=guides;
        m_monoCapturedFrame.framegenInputs[eye]=guides;
#include "publish_main_capture.inc"
    }
#endif
    void ReleaseCapturedRead(ID3D12Resource* resource,uint64_t fence){
        Check(fence==0,"CPU fixture attempted GPU-owned retirement");
        {std::lock_guard lock(m_presentMutex);m_captureLeases.Unpin(reinterpret_cast<uintptr_t>(resource));++releases;}
        resource->Release();
    }
};
static OpenXRManager* validatingManager{};
static bool liveScope=true;
static bool threadedValidation=false;
namespace cvr::camera {
bool ValidateMainEyeSourceReceipt(const cvr::framegen::MainEyeSourceReceipt& receipt) {
    if(validatingManager&&!threadedValidation){const bool unlocked=validatingManager->m_presentMutex.try_lock();if(unlocked)validatingManager->m_presentMutex.unlock();Check(unlocked,"live receipt validation held presentMutex");}
    return liveScope && bool(receipt);
}
}
#if HAVE_CATALOGUE
#include "collect_main_captures.inc"
#else
// Baseline model of the current single latest tuple + FromMainFrame completion
// gate. Its lost-ready-source counterexample is kept identical after the new
// implementation is available; GREEN includes the exact production collector.
std::vector<Frame> OpenXRManager::CollectReadyMainCaptures(const MainEyeSourceReceipt&) {
    if(!latest.colorCompletion.HasScheduledCopySource())return {};
    const auto done=latest.colorCompletion.fence->GetCompletedValue();
    return done!=UINT64_MAX && done>=latest.colorCompletion.fenceValue?std::vector<Frame>{latest}:std::vector<Frame>{};
}
#endif
struct Fixture {
    ComPtr<CpuDevice> device;ComPtr<CpuFence> fence;ComPtr<CpuQueue> queue;
    std::array<ComPtr<CpuResource>,8> resources;
    OpenXRManager owner;
    Fixture(uint32_t count=4){
        device.Attach(new CpuDevice);fence.Attach(new CpuFence(device.Get()));queue.Attach(new CpuQueue(device.Get()));
        owner.m_monoPoolCount=count;
        for(unsigned i=0;i<count;++i){resources[i].Attach(new CpuResource(device.Get()));owner.m_monoPool[i]=resources[i].Get();}
#if HAVE_CATALOGUE
        std::array<uintptr_t,8> addresses{};for(unsigned i=0;i<count;++i)addresses[i]=reinterpret_cast<uintptr_t>(resources[i].Get());
        owner.m_mainCaptureCatalogue.Reset(3,std::span(addresses).first(count));
#endif
        validatingManager=&owner;liveScope=true;
    }
    ~Fixture(){validatingManager=nullptr;}
    Frame Make(unsigned slot,int64_t id,uint64_t ready){
        Frame value;const auto eye=unsigned(id&1);value.color[eye]=resources[slot].Get();value.engineView[eye]=1;
        value.identity.serial=uint64_t(id+10);value.identity.origin=5;value.identity.pose[eye]=uint64_t(id+100);
        value.nativeStamp.resourceEpoch=3;value.nativeStamp.eyes[eye].mainEyeSource=Scope(id);
        value.nativeStamp.eyes[eye].sourceSerial=value.identity.serial;
        value.nativeStamp.eyes[eye].engineView=1;
        value.colorCompletion=CaptureCompletion::AfterSignal(fence.Get(),ready,S_OK,{},0);
        FrameProjectionMetadata projection;projection.queue=queue.Get();projection.poseId=value.identity.pose[eye];
        value.projection[eye]=std::move(projection);return value;
    }
    void Publish(unsigned slot,int64_t id,uint64_t ready){
        auto value=Make(slot,id,ready);
#if HAVE_CATALOGUE
        std::lock_guard lock(owner.m_presentMutex);
        auto ticket=owner.m_mainCaptureCatalogue.BeginWrite(slot,owner.m_mainCaptureCatalogue.Generation());
        Check(owner.m_mainCaptureCatalogue.Publish(ticket,Scope(id),value.identity.serial,std::move(value)),"publication failed");
#else
        owner.latest=std::move(value);
#endif
    }
    unsigned Pins()const {unsigned result{};for(const auto& resource:resources)result+=owner.m_captureLeases.Readers(reinterpret_cast<uintptr_t>(resource.Get()));return result;}
};
static void LatestPendingMustNotHideReadyOtherEye(){
    Fixture f;f.fence->completed=20;f.Publish(0,2,20);f.Publish(1,3,21);
    auto batch=f.owner.CollectReadyMainCaptures(Scope(4));
    Check(batch.size()==1 && batch[0].nativeStamp.eyes[0].mainEyeSource.sourceId==2,
        "latest pending right hid earlier GPU-ready eligible left");
#if HAVE_CATALOGUE
    Check(f.Pins()==1,"pending color was pinned");batch.clear();Check(f.Pins()==0,"rejected ready batch did not unpin");
    f.fence->completed=21;batch=f.owner.CollectReadyMainCaptures(Scope(4));
    Check(batch.size()==2&&f.Pins()==2,"both ready eyes were not exposed");
    batch.clear();Check(f.Pins()==0,"unselected batch held colors after immediate release");
#endif
}
#if HAVE_CATALOGUE
static void OverwriteResetAndScope(){
    Fixture f;f.fence->completed=100;f.Publish(0,0,1);f.Publish(1,1,2);
    auto& c=f.owner.m_mainCaptureCatalogue;
    auto keys=c.ReadyKeys(Scope(2),[](const auto&){return true;});Check(keys.size==2,"two metadata publications lost");
    const auto old=keys.keys[0];const auto attempt=c.Generation();
    const auto overwrite=c.BeginWrite(old.slot,attempt);
    Check(!c.Find(old),"overwritten version survived at reserve before GPU write");
    Check(c.ReadyKeys(Scope(2),[](const auto&){return true;}).size==1,"reserved cell stayed discoverable");
    c.Clear();auto value=f.Make(overwrite.slot,4,4);
    Check(!c.Publish(overwrite,Scope(4),14,value),"reset/toggle resurrected an in-flight reservation");
    auto beganBeforeReset=c.BeginWrite(0,attempt);
    Check(!c.Publish(beganBeforeReset,Scope(4),14,value),"reservation after reset accepted old attempt generation");
    f.Publish(0,4,4);Check(c.ReadyKeys(Scope(4),[](const auto&){return true;}).size==1,"fresh generation did not publish");
    auto foreign=Scope(4);++foreign.latchRevision;
    Check(f.owner.CollectReadyMainCaptures(foreign).empty()&&f.Pins()==0,"foreign latch retained/pinned a frame");
    std::array<uintptr_t,4> same{};for(unsigned i=0;i<4;++i)same[i]=reinterpret_cast<uintptr_t>(f.resources[i].Get());
    const auto prePool=c.ReadyKeys(Scope(4),[](const auto&){return true;}).keys[0];
    c.Reset(4,same);Check(!c.Find(prePool),"pool recreation reused an old address/key");
    Check(c.ReadyKeys(Scope(4),[](const auto&){return true;}).size==0,"retired pool entered active catalog");
}
static void ReadinessAndPins(){
    Fixture f;f.fence->completed=4;for(unsigned i=0;i<4;++i)f.Publish(i,i,i+1);
    Check(f.Pins()==0,"catalogue metadata pinned color cells");
    auto batch=f.owner.CollectReadyMainCaptures(Scope(4));Check(batch.size()==4&&f.Pins()==4,"bounded temporary READY batch did not pin actual ready cells");
    for(unsigned i=0;i<4;++i)Check(!f.owner.m_captureLeases.BeginWrite(reinterpret_cast<uintptr_t>(f.resources[i].Get())),"writer overwrote a ready batch reader");
    Frame accepted=std::move(batch[0]);batch.clear();Check(f.Pins()==1,"unselected READY frames were retained");accepted={};Check(f.Pins()==0,"accepted owner never unpinned");
    f.fence->completed=UINT64_MAX;Check(f.owner.CollectReadyMainCaptures(Scope(4)).empty()&&f.Pins()==0,"device removal became GPU readiness");
    f.fence->completed=4;
    const auto key=reinterpret_cast<uintptr_t>(f.resources[2].Get());Check(f.owner.m_captureLeases.BeginWrite(key),"reserve failed");
    batch=f.owner.CollectReadyMainCaptures(Scope(4));Check(batch.size()==3&&f.owner.m_captureLeases.Readers(key)==0,"writer exclusion was not atomic with pin");
    batch.clear();f.owner.m_captureLeases.EndWrite(key);
    liveScope=false;batch=f.owner.CollectReadyMainCaptures(Scope(4));Check(batch.empty()&&f.Pins()==0,"late scope revocation retained acquired batch");
}
static void GuideRecordIsNotRetained(){
    Fixture f;auto guides=std::make_shared<Inputs>();guides->queue=f.queue.Get();guides->view=1;guides->poseId=55;
    std::weak_ptr<Inputs> weak=guides;auto metadata=SnapshotFrameProjection(*guides);guides.reset();
    Check(weak.expired()&&metadata.poseId==55,"projection snapshot retained the four-slot Inputs record");
    f.fence->completed=1;f.Publish(0,0,1);auto frames=f.owner.CollectReadyMainCaptures(Scope(1));
    Check(frames.size()==1&&!frames[0].inputs[0]&&!frames[0].inputs[1],"catalogue retained guide records");
}
static void ScopeAndSignalGuards(){
    Fixture f;f.fence->completed=10;f.Publish(0,0,1);
    for(unsigned i=0;i<6;++i){auto scope=Scope(1);switch(i){
        case 0:++scope.requestRevision;break;case 1:++scope.latchRevision;break;
        case 2:++scope.clockGeneration;break;case 3:++scope.epochs.renderer;break;
        case 4:++scope.epochs.gameWorld;break;case 5:++scope.epochs.hmdWorld;break;
    }Check(f.owner.CollectReadyMainCaptures(scope).empty()&&f.Pins()==0,"foreign owner/epoch pinned ready pixels");}
    auto& c=f.owner.m_mainCaptureCatalogue;
    auto write=c.BeginWrite(1,c.Generation());auto failed=f.Make(1,1,2);
    failed.colorCompletion=CaptureCompletion::AfterSignal(f.fence.Get(),2,E_FAIL,{},0);
    Check(c.Publish(write,Scope(1),11,std::move(failed)),"fixture failed to publish failed-Signal metadata");
    auto batch=f.owner.CollectReadyMainCaptures(Scope(2));Check(batch.size()==1&&batch[0].color[0],"later high fence made failed Signal ready");
    batch.clear();f.owner.m_captureResourceEpoch=9;
    Check(f.owner.CollectReadyMainCaptures(Scope(2)).empty()&&f.Pins()==0,"resource epoch change admitted old live-cell metadata");
}
static void ConcurrentProducerAndReader(){
    Fixture f;std::array<std::atomic<uint64_t>,4> pixels{};
    for(unsigned i=0;i<4;++i){f.Publish(i,i,i+1);pixels[i]=i+10;}f.fence->completed=4;
    std::atomic<bool> start{},done{},bad{};std::atomic<unsigned> reads{},writes{},skips{};
    threadedValidation=true;
    std::thread producer([&]{
        while(!start.load(std::memory_order_acquire))std::this_thread::yield();
        for(int64_t id=4;id<1504;++id){
            const unsigned slot=unsigned(id)%4;const auto resource=reinterpret_cast<uintptr_t>(f.resources[slot].Get());
            decltype(f.owner.m_mainCaptureCatalogue)::Key key;
            {std::lock_guard lock(f.owner.m_presentMutex);
                if(!f.owner.m_captureLeases.BeginWrite(resource)){++skips;continue;}
                key=f.owner.m_mainCaptureCatalogue.BeginWrite(slot,f.owner.m_mainCaptureCatalogue.Generation());
            }
            // CPU simulation of the actual copy followed by its queue Signal.
            // A reader may hold other cells while this reserved one is written.
            pixels[slot].store(uint64_t(id+10),std::memory_order_release);
            f.fence->completed.store(uint64_t(id+1),std::memory_order_release);
            auto value=f.Make(slot,id,uint64_t(id+1));
            {std::lock_guard lock(f.owner.m_presentMutex);
                if(id%31==0)f.owner.m_mainCaptureCatalogue.Clear(); // reset between reserve/publish
                const bool published=f.owner.m_mainCaptureCatalogue.Publish(key,Scope(id),uint64_t(id+10),std::move(value));
                if((id%31!=0)!=published)bad=true;
                f.owner.m_captureLeases.EndWrite(resource);
            }
            ++writes;std::this_thread::yield();
        }
        done.store(true,std::memory_order_release);
    });
    do{
        auto batch=f.owner.CollectReadyMainCaptures(Scope(2000));
        if(batch.size()>4)bad=true;
        for(const auto& frame:batch){const auto eye=frame.color[0]?0u:1u;
            unsigned slot=4;for(unsigned i=0;i<4;++i)if(frame.color[eye]==f.resources[i].Get())slot=i;
            if(slot==4 || pixels[slot].load(std::memory_order_acquire)!=frame.identity.serial)bad=true;
            ++reads;
        }
        batch.clear();start.store(true,std::memory_order_release);std::this_thread::yield();
    }while(!done.load(std::memory_order_acquire));
    producer.join();threadedValidation=false;
    std::cout<<"concurrent_reads="<<reads.load()<<" writes="<<writes.load()<<" bounded_reader_skips="<<skips.load()<<" invalid="<<bad.load()<<" pins="<<f.Pins()<<'\n';
    Check(!bad&&reads.load()>0&&writes.load()>0&&f.Pins()==0,"overwrite/reset/readiness race broke versioned acquisition or left pins");
}
static void EightLiveCellsAndExactPublication(){
    Fixture f(8);f.fence->completed=8;
    for(unsigned i=0;i<8;++i)f.Publish(i,i,i+1);
    auto batch=f.owner.CollectReadyMainCaptures(Scope(9));Check(batch.size()==8&&f.Pins()==8,"ready batch exceeded/lost active eight-cell bound");
    for(size_t i=1;i<batch.size();++i){const auto a=batch[i-1].color[0]?0u:1u,b=batch[i].color[0]?0u:1u;
        Check(batch[i-1].nativeStamp.eyes[a].mainEyeSource.sourceId>batch[i].nativeStamp.eyes[b].mainEyeSource.sourceId,"ready batch was not newest-first");}
    batch.clear();Check(f.Pins()==0,"eight temporary READY pins persisted");
    auto guide=std::make_shared<Inputs>();guide->queue=f.queue.Get();guide->poseId=777;guide->view=1;
    std::weak_ptr<Inputs> weak=guide;auto& catalogue=f.owner.m_mainCaptureCatalogue;
    const auto key=catalogue.BeginWrite(0,catalogue.Generation());auto value=f.Make(0,10,8);
    f.owner.PublishExact(key,value,guide);
    Check(guide.use_count()==1,"actual publication retained a guide Record in catalogue/latest");guide.reset();
    Check(weak.expired(),"actual publication pinned the four-slot guide pool");
    const auto* entry=catalogue.Find(key);
    Check(entry && entry->imageGeneration==71 && entry->payload.projection[0] &&
          entry->payload.projection[0]->poseId==777 && !entry->payload.inputs[0]&&!entry->payload.lease,
          "actual publication lost immutable metadata or retained source ownership");
    auto stale=catalogue.BeginWrite(1,catalogue.Generation());catalogue.Clear();
    f.owner.PublishExact(stale,f.Make(1,11,8),{});
    Check(!catalogue.Find(stale),"actual publication resurrected before-reset writer");
    auto disabled=catalogue.BeginWrite(1,catalogue.Generation());f.owner.m_monoSubmitEnabled=false;
    f.owner.PublishExact(disabled,f.Make(1,11,8),{});
    Check(!catalogue.Find(disabled),"disabled mono published AER catalogue metadata");
    auto legacyGuide=std::make_shared<Inputs>();legacyGuide->queue=f.queue.Get();
    auto legacy=catalogue.BeginWrite(1,catalogue.Generation());f.owner.m_monoSubmitEnabled=true;
    f.owner.PublishExact(legacy,f.Make(1,11,8),legacyGuide,false);
    Check(!catalogue.Find(legacy) && f.owner.m_monoCapturedFrame.framegenInputs[1]==legacyGuide,"legacy latest Inputs behavior changed");
}
#endif
template<class Catalogue>
static void OriginalPendingPublicationContract(Fixture& f,Catalogue& c){
    if constexpr(requires {c.SetOrderedMode(true);}){
        c.SetOrderedMode(true);
        f.Publish(0,0,10);f.Publish(1,1,2);f.Publish(2,2,2);
        f.fence->completed=2;
        auto pending=c.ReadyKeys(Scope(3),[&](const auto& entry){return entry.payload.colorCompletion.fenceValue<=f.fence->completed;});
        Check(pending.size==1&&c.Find(pending.keys[0])->receipt.sourceId==1,"later ready left bypassed its unfinished FIFO head");
        Check(!c.CanOverwrite(0)&&!c.BeginWrite(0,c.Generation()),"producer overwrote an unclaimed queued image");
        Check(c.Claim(pending.keys[0])&&c.CanOverwrite(1),"ready ownership could not transfer out of producer queue");
        Check(!c.Claim(pending.keys[0]),"same publication was claimed twice");
        f.fence->completed=10;
        pending=c.ReadyKeys(Scope(3),[](const auto&){return true;});
        Check(pending.size==2&&c.Find(pending.keys[0])->receipt.sourceId==0&&c.Find(pending.keys[1])->receipt.sourceId==2,
              "producer queue was not ordered by the actual game source");
        auto replacement=Scope(4);++replacement.latchRevision;c.DiscardOutsideScope(replacement);
        Check(c.CanOverwrite(0)&&c.CanOverwrite(2)&&c.ReadyKeys(replacement,[](const auto&){return true;}).size==0,
              "scope revocation kept unclaimed old queue cells reserved");
    }else Check(false,"producer catalogue has no protected ordered publication handoff");
}
static void OrderedCollectorClaimsOnce(){
    Fixture f(8);auto& c=f.owner.m_mainCaptureCatalogue;c.SetOrderedMode(true);
    f.Publish(0,0,1);f.Publish(3,1,2);f.fence->completed=2;
    auto batch=f.owner.CollectReadyMainCaptures(Scope(3));
    Check(batch.size()==2&&f.Pins()==2,"ordered collector did not transfer both ready publications into leases");
    Check(f.owner.CollectReadyMainCaptures(Scope(3)).empty(),"producer publication was claimed more than once");
    Check(!f.owner.m_captureLeases.BeginWrite(reinterpret_cast<uintptr_t>(f.resources[0].Get())),"claim released the pixel reservation before its consumer lease");
    batch.clear();Check(f.Pins()==0&&c.CanOverwrite(0)&&c.CanOverwrite(3),"retired claims did not free producer cells");
    unsigned left=0,right=0;
    for(uint32_t slot=0;slot<8;++slot){left+=cvr::capture::MainEyeQueueSlot(slot,0);right+=cvr::capture::MainEyeQueueSlot(slot,1);
        Check(!cvr::capture::MainEyeQueueSlot(slot,2),"invalid physical eye owns producer cells");}
    Check(left==3&&right==3&&!cvr::capture::MainEyeQueueSlot(6,0)&&!cvr::capture::MainEyeQueueSlot(7,1),"original three-cell eye partition changed");
}
int main(){try{
#if HAVE_CATALOGUE
    {Fixture f;OriginalPendingPublicationContract(f,f.owner.m_mainCaptureCatalogue);}
    OrderedCollectorClaimsOnce();
#endif
    LatestPendingMustNotHideReadyOtherEye();
#if HAVE_CATALOGUE
    OverwriteResetAndScope();ReadinessAndPins();GuideRecordIsNotRetained();ScopeAndSignalGuards();ConcurrentProducerAndReader();EightLiveCellsAndExactPublication();
#endif
    std::cout<<"PASS main capture catalogue checks="<<checks<<" gpu_execution=0\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}

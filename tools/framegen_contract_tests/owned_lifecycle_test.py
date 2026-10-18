"""Execute the real OpenXR owned-generator routing body with controlled owners."""
from pathlib import Path
import sys
root=Path(__file__).resolve().parents[2]
source=(root/'src/Runtimes/OpenXRGeneration.cpp').read_text(encoding='utf-8-sig')
start=source.index('bool OpenXRManager::UpdateOwnedGeneration(')
opening=source.index('{',start); end=opening+1; depth=1
while depth:
    depth+=(source[end]=='{')-(source[end]=='}');end+=1
body=source[start:end]
fixture=r'''
#include "Framegen/AsyncRetirement.hpp"
#include <memory>
#include <optional>
#include <string>
#include <atomic>
#include <thread>
#include <cstdio>
#include <cstdlib>
using namespace std::chrono_literals;
std::atomic<int> created{},destroyed{},stopped{};
std::atomic<bool> blockStop{true},cleanStop{true};
void Log(const char*,...){}
namespace cpvr {
struct GenerationPolicy{int count{};bool operator==(const GenerationPolicy&)const=default;};
struct GenerationWorker{
    GenerationWorker(){++created;}~GenerationWorker(){++destroyed;}
    bool configure(GenerationPolicy){return true;}
    void stop(){++stopped;while(blockStop.load())std::this_thread::sleep_for(1ms);}
    bool stopped_cleanly()const{return cleanStop.load();}
};
}
namespace cvr::framegen {
enum class Backend{FidelityFX,Nvidia,OwnNvidia};
struct Settings{Backend backend=Backend::OwnNvidia;bool enabled{};int ownGeneratedFrames=1;bool aerV2{};};
cpvr::GenerationPolicy MakeOwnedGenerationPolicy(const Settings&s){return {s.enabled?s.ownGeneratedFrames+1:0};}
void Status(const char*,bool=false){}
struct NativeGenerationBridge{NativeGenerationBridge(cpvr::GenerationWorker&,int,std::function<void(const std::string&)>){}};
}
std::optional<int> OwnedConfig(cpvr::GenerationPolicy){return 1;}
struct Presenter{bool allowDetach=true;bool SetNativeBridge(std::shared_ptr<cvr::framegen::NativeGenerationBridge> p){return bool(p)||allowDetach;}};
struct RetirementProbe:cvr::framegen::AsyncRetirement{
    bool failNext{};
    bool Begin(std::function<bool()> stop)noexcept{if(failNext){failNext=false;return false;}return AsyncRetirement::Begin(std::move(stop));}
};
struct OpenXRManager {
    std::unique_ptr<cpvr::GenerationWorker> m_ownGeneration;
    std::shared_ptr<cvr::framegen::NativeGenerationBridge> m_ownGenerationBridge;
    RetirementProbe m_ownRetirement;
    bool m_ownGenerationConnected=false;
    bool m_ownGenerationStopping=false;
    std::optional<cpvr::GenerationPolicy> m_ownGenerationPolicy;
    uint64_t m_ownAerSourceGeneration{},m_ownAerModeGeneration{};
    Presenter m_framegen;
    bool UpdateOwnedGeneration(const cvr::framegen::Settings&);
};
@@BODY@@
void Check(bool okay,const char* why){if(!okay){blockStop=false;std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}}
template<class F> bool Until(F f){for(unsigned i=0;i<1000;++i){if(f())return true;std::this_thread::sleep_for(1ms);}return false;}
void LaunchFailure(bool aer){
    OpenXRManager manager;cvr::framegen::Settings on;on.enabled=true;on.aerV2=aer;auto off=on;off.enabled=false;
    const int initial=created;Check(manager.UpdateOwnedGeneration(on),"launch-failure fixture did not start");
    manager.m_ownRetirement.failNext=true;
    Check(!manager.UpdateOwnedGeneration(off),"failed async launch advertised a completed stop");
    blockStop=true;
    Check(!manager.UpdateOwnedGeneration(on)&&!manager.m_ownGenerationConnected&&!manager.m_ownGenerationBridge&&created==initial+1,
        "immediate On after launch failure admits a missing bridge or overlaps a worker");
    blockStop=false;
    Check(Until([&]{return manager.UpdateOwnedGeneration(on);})&&manager.m_ownGenerationBridge,"retirement retry did not recreate a usable bridge");
    manager.UpdateOwnedGeneration(off);Check(Until([&]{return manager.UpdateOwnedGeneration(off);}),"fixture stop did not finish");
}
int main(){
    using namespace cvr::framegen;
    OpenXRManager manager;Settings off;
    Check(manager.UpdateOwnedGeneration(off)&&created==0,"FG-off cold start allocates an unused NVIDIA owner");
    auto on=off;on.enabled=true;
    Check(manager.UpdateOwnedGeneration(on)&&created==1&&manager.m_ownGenerationConnected,"enabled owner not connected");
    manager.m_framegen.allowDetach=false;
    Check(!manager.UpdateOwnedGeneration(off)&&stopped==0,"backend stopped before GPU consumers retired");
    manager.m_framegen.allowDetach=true;
    Check(!manager.UpdateOwnedGeneration(off),"unfinished stop advertised ready");
    Check(Until([]{return stopped==1;}),"old worker never stopped");
    Check(!manager.UpdateOwnedGeneration(on)&&created==1,"reenable overlaps stopping worker");
    blockStop=false;
    Check(Until([&]{return manager.UpdateOwnedGeneration(on);})&&created==2&&destroyed==1,"clean stop did not allow a fresh owner");
    auto ffx=on;ffx.backend=Backend::FidelityFX;
    manager.UpdateOwnedGeneration(ffx);
    Check(Until([&]{return manager.UpdateOwnedGeneration(ffx);})&&!manager.m_ownGeneration&&!manager.m_ownGenerationBridge,"backend switch retains old NVIDIA worker");
    Check(manager.UpdateOwnedGeneration(on)&&created==3,"new NVIDIA owner not created after FFX");
    LaunchFailure(false);LaunchFailure(true);const int beforeFailure=created;
    cleanStop=false;manager.UpdateOwnedGeneration(off);
    Check(Until([&]{manager.UpdateOwnedGeneration(off);return manager.m_ownRetirement.Poll()==RetirementState::Failed;}),"unsafe stop not surfaced");
    Check(!manager.UpdateOwnedGeneration(on)&&created==beforeFailure&&manager.m_ownGeneration,"unsafe resources not retained or new owner started");
    std::puts("PASS production owner lifecycle: cold off, GPU drain, async stop, reenable, backend switch, unsafe retention");
}
'''
Path(sys.argv[1]).write_text(fixture.replace('@@BODY@@',body),encoding='utf-8')

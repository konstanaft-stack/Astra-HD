#include <Windows.h>
#include "Framegen/VrcamOffReadback.hpp"
#include "Framegen/AerScenePolicy.hpp"
#include <memory>
#include <cstdio>
#include <stdexcept>
using namespace cvr::framegen;
struct Entity {uint32_t cameras=62;bool readable=true,off=true;};
using Handle=std::shared_ptr<Entity>;
static unsigned checks;
static void Check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
static VrcamOffReadback Inspect(const Handle& p){return p?VrcamOffReadback{p->cameras,p->readable,p->off}:VrcamOffReadback{};}
int main(){try{
    VrcamOffEntities<Handle> entities;auto player=std::make_shared<Entity>();
    auto result=entities.Observe(1,player,Inspect);
    Check(bool(result)&&result.inspected==62,"no native readback for the actual disabled cameras without a Lua callback");
    AerScenePolicy mode;mode.UpdateRuntime({true,true,5,7,1000000000});
    SourceSceneFrame frame{1,1,123,true,false};Check(!mode.OnSceneFrame(frame).active,"AER activated without component proof");
    Check(mode.ReportVrcamOff(mode.OffRequestToken(),result.inspected,bool(result)),"native readback did not acknowledge current token");
    frame.ordinal=2;auto left=mode.OnSceneFrame(frame);
    Check(left.active&&left.sourceId==0&&left.eye==AerV2Eye::Left,"native proof did not start the actual source stream");
    player->off=false;auto replacement=std::make_shared<Entity>();std::weak_ptr<Entity> old=player;
    result=entities.Observe(1,replacement,Inspect);player.reset();
    Check(!result,"still-enabled previous player cameras were ignored");Check(!old.expired(),"pending old entity lost its lifetime");
    Check(!mode.ReportVrcamOff(mode.OffRequestToken(),result.inspected,bool(result)),"lost proof was accepted");
    frame.ordinal=3;Check(!mode.OnSceneFrame(frame).active,"camera remained active after old-player off proof failed");
    old.lock()->off=true;result=entities.Observe(1,replacement,Inspect);
    Check(bool(result)&&old.expired(),"observed-disabled old entity did not retire");
    replacement->readable=false;Check(!entities.Observe(1,replacement,Inspect),"unreadable component state became off proof");
    replacement->readable=true;replacement->cameras=0;Check(!entities.Observe(1,replacement,Inspect),"empty component scan became off proof");
    replacement->cameras=62;Check(!entities.Observe(1,Handle{},Inspect),"missing player became off proof");
    VrcamOffEntities<Handle,2> bounded;auto first=std::make_shared<Entity>();first->off=false;
    bounded.Observe(4,first,Inspect);
    for(int i=0;i<3;++i){auto next=std::make_shared<Entity>();next->off=false;bounded.Observe(4,next,Inspect);}
    Check(!bounded.Observe(4,std::make_shared<Entity>(),Inspect),"pending entity overflow was silently accepted");
    Check(bool(bounded.Observe(5,std::make_shared<Entity>(),Inspect)),"new world did not revoke the prior world's pending entities");
    Check(!bounded.Observe(4,std::make_shared<Entity>(),Inspect),"late old-world observation rolled back entity ownership");
    Check(!bounded.Observe(0,std::make_shared<Entity>(),Inspect),"unknown world became proof");
    auto detached=std::make_shared<Entity>();std::weak_ptr<Entity> detachLife=detached;
    bounded.Observe(6,detached,Inspect);detached.reset();bounded.Reset(6);
    Check(detachLife.expired(),"world detach retained an engine entity until XR teardown");
    std::printf("PASS %u native VRCAM readback checks\n",checks);return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}}

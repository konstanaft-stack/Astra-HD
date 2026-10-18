// Exact lifecycle and real-receipt gates are extracted by the adjacent runner.
#include "Overlay/PpdPanel.hpp"
#include <array>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>
namespace fixture {
unsigned checks=0,panelInvalidations=0,ownerUpdates=0;
struct Context {bool requested=true,ready=true,cameraActive=true;} context;
struct Runtime {bool requested,ready;uint64_t renderer,origin;int64_t time;};
Runtime lastRuntime{};
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
struct HeldPair {
    unsigned resets=0;std::array<std::shared_ptr<int>,2> eyes{std::make_shared<int>(0),std::make_shared<int>(1)};
    void Reset(){++resets;eyes={};}
};
struct Pass {unsigned collects=0;void Collect(){++collects;}};
struct Framegen {unsigned resets=0;void Reset(){++resets;}};
}
namespace cvr::framegen {
struct Settings {bool aerV2=true;};
Settings GetSettings(){return {};}
void UpdateAerModeRuntime(fixture::Runtime value){++fixture::ownerUpdates;fixture::lastRuntime=value;fixture::context.ready=value.ready;}
fixture::Context ReadAerModeSnapshot(){return fixture::context;}
}
namespace cvr::ppd {void InvalidatePpdSourceContext(){++fixture::panelInvalidations;}}
#include "aer-context-sites.hpp"
static void ContextCases(){
    using namespace fixture;
    struct Case {const char* name;bool earlyReturn,earlyContinue,monoReady;unsigned views;bool requested,ready,menu;bool invalidate,ownerUnavailable;};
    const Case cases[]={
        {"early return",true,false,true,2,true,true,false,true,true},
        {"early continue",false,true,true,2,true,true,false,true,true},
        {"mono resources unavailable",false,false,false,2,true,true,false,true,true},
        {"locate views failed",false,false,true,0,true,true,false,true,true},
        {"incomplete stereo",false,false,true,1,true,true,false,true,true},
        {"disabled mode",false,false,true,2,false,true,false,true,false},
        {"owner not ready",false,false,true,2,true,false,false,true,false},
        {"menu mode",false,false,true,2,true,true,true,true,false},
        {"healthy accepted context",false,false,true,2,true,true,false,false,false}
    };
    for(const auto& test:cases){
        ActualManager manager;context={test.requested,test.ready,true};panelInvalidations=ownerUpdates=0;lastRuntime={};
        std::weak_ptr<int> left=manager.m_aerRealPresenter.eyes[0],right=manager.m_aerRealPresenter.eyes[1];
        manager.Run(test.earlyReturn,test.earlyContinue,test.monoReady,test.views,test.menu);
        Check((panelInvalidations==1)==test.invalidate,test.name);
        Check((manager.m_aerRealPresenter.resets==1)==test.invalidate,test.name);
        Check((left.expired()&&right.expired())==test.invalidate,"held pair lifetime mismatch");
        Check(manager.pass.collects==1,"pass was not collected exactly once");
        Check((ownerUpdates==1)==test.ownerUnavailable,"owner-unavailable update mismatch");
        Check((manager.m_framegen.resets==1)==test.ownerUnavailable,"framegen reset mismatch");
        if(test.ownerUnavailable)Check(lastRuntime.requested && !lastRuntime.ready && lastRuntime.renderer==0 &&
            lastRuntime.origin==42 && lastRuntime.time==0,"invalidated runtime invented ready/epoch/time");
    }
}
static void ScopeCases(){
    using namespace fixture;using namespace cvr::ppd;
    PpdPanelSnapshot ppd;ppd.calibrated=true;ppd.measuredSource.scope={1,2,3,4,5,6};
    PpdSourceObservation same;same.scope=ppd.measuredSource.scope;
    const PpdPanelStage stages[]={PpdPanelStage::Ready,PpdPanelStage::Requested,PpdPanelStage::WaitingNative,
        PpdPanelStage::WaitingSource,PpdPanelStage::WaitingEyes,PpdPanelStage::Applied,PpdPanelStage::Error};
    for(auto stage:stages){
        ppd.stage=stage;
        const bool waiting=stage==PpdPanelStage::Requested||stage==PpdPanelStage::WaitingNative||
            stage==PpdPanelStage::WaitingSource||stage==PpdPanelStage::WaitingEyes;
        Check(ActualNeedRealReceipt(ppd,same)==waiting,"unchanged calibrated scope altered native eligibility");
        Check(ActualNeedRealReceipt(ppd,std::nullopt),"missing source observation permitted native receipt");
        for(unsigned member=0;member<6;++member){
            auto changed=same;
            switch(member){case 0:++changed.scope.resourceEpoch;break;case 1:++changed.scope.rendererEpoch;break;
            case 2:++changed.scope.gameWorldEpoch;break;case 3:++changed.scope.hmdWorldEpoch;break;
            case 4:++changed.scope.modeGeneration;break;default:++changed.scope.sourceGeneration;break;}
            Check(ActualNeedRealReceipt(ppd,changed),"changed measured scope bypassed baseline real acknowledgement");
        }
        auto uncalibrated=ppd;uncalibrated.calibrated=false;
        Check(ActualNeedRealReceipt(uncalibrated,same),"uncalibrated native selection accepted");
    }
}
int main(){unsigned passed=0,failed=0;
    const std::pair<const char*,void(*)()> tests[]={{"early_exit_and_unready_cleanup_9_cases",ContextCases},{"real_receipt_scope_and_stage_63_cases",ScopeCases}};
    for(const auto& [name,test]:tests){try{test();++passed;std::cout<<"PASS "<<name<<'\n';}catch(const std::exception& e){++failed;std::cout<<"FAIL "<<name<<": "<<e.what()<<'\n';}}
    std::cout<<"RESULT passed="<<passed<<" failed="<<failed<<" checks="<<fixture::checks<<" GPU=not-used\n";
    return failed?1:0;
}

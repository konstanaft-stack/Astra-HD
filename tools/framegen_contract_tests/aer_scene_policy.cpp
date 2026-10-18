#include <Windows.h>
#include "Framegen/AerScenePolicy.hpp"
#include <cstdio>
#include <cstdlib>
using namespace cvr::framegen;
static unsigned checks;
static void check(bool ok,const char* label){++checks;if(!ok){std::fprintf(stderr,"FAIL %s\n",label);std::exit(1);}}
int main(){
    AerScenePolicy owner;
    SourceSceneFrame scene{1,1,123,true,false};
    AerRuntimeSnapshot runtime{true,true,5,7,1'000'000'000};
    owner.UpdateRuntime(runtime);
    auto d=owner.OnSceneFrame(scene);
    check(!d.active && owner.OffRequestToken()!=0,"no camera without actual VRCAM-off acknowledgement");
    const auto token=owner.OffRequestToken();
    check(!owner.ReportVrcamOff(token+1,63,true),"foreign token rejected");
    check(!owner.ReportVrcamOff(token,0,true),"empty component scan is not off proof");
    check(owner.ReportVrcamOff(token,63,true),"actual component readback accepted");
    check(!owner.OnSceneFrame(scene).active,"ack does not activate halfway through same game frame");
    scene.ordinal=2;d=owner.OnSceneFrame(scene);
    check(d.active && d.sourceId==0 && d.eye==AerV2Eye::Left,"first complete frame renders actual left source0");
    scene.ordinal=3;d=owner.OnSceneFrame(scene);
    check(d.active && d.sourceId==1 && d.eye==AerV2Eye::Right,"next frame renders actual right source1");
    auto epoch=d.epochs;const auto generation=d.generation;
    for(int i=0;i<50;++i)owner.ObserveAccepted(generation,epoch,{true,1'000'000'000+i*10'000'000,{i*2,i*2+1},2});
    check(owner.Fit().valid && owner.Fit().samples==100,"accepted two-eye submits update actual 100-sample source clock");
    owner.ObserveAccepted(generation,epoch,{false,2'000'000'000,{100,101},2});
    check(owner.Fit().samples==100,"failed EndFrame is not a sample");
    check(!owner.ReportVrcamOff(token,63,false),"loss of actual disable proof is rejected");
    check(owner.Generation()!=generation && owner.Fit().samples==0 && !owner.Fit().valid,
          "loss of confirmed off resets generation and fitted source clock");
    check(owner.OffRequestToken()!=0 && owner.OffRequestToken()!=token,"reconfirmation has a fresh accessible token");
    const auto lostGeneration=owner.Generation();
    owner.ReportVrcamOff(token,63,false);
    check(owner.Generation()==lostGeneration,"late failed old-token reports cannot reset the replacement");
    runtime.rendererEpoch=6;owner.UpdateRuntime(runtime);
    check(owner.OffRequestToken()!=token && !owner.Fit().valid,"resize revokes off token and source clock");
    owner.ObserveAccepted(generation,epoch,{true,2'000'000'000,{100,101},2});
    check(owner.Fit().samples==0,"late accepted submit cannot contaminate resized clock");
    check(!owner.ReportVrcamOff(token,63,true),"late old off acknowledgement cannot enable new epoch");
    scene.ordinal=4;d=owner.OnSceneFrame(scene);
    check(!d.active && d.invalidate,"resize invalidates camera receipt even before new ack");
    owner.ReportVrcamOff(owner.OffRequestToken(),63,true);scene.ordinal=5;d=owner.OnSceneFrame(scene);
    check(d.active && d.sourceId==0 && d.epochs.renderer==6,"new resize source generation cold starts");
    scene.paused=true;scene.valid=false;owner.OnSceneFrame(scene);
    check(owner.OffRequestToken()==0,"paused or absent scene cannot activate AER");
    scene.paused=false;scene.valid=true;scene.ordinal=6;d=owner.OnSceneFrame(scene);
    check(!d.active && !owner.Fit().valid,"resume requires new disable acknowledgement");
    auto resumedToken=owner.OffRequestToken();owner.ReportVrcamOff(resumedToken,63,true);
    scene.worldEpoch=2;scene.ordinal=1;d=owner.OnSceneFrame(scene);
    check(!d.active && owner.OffRequestToken()!=resumedToken,"new world rejects previous entity acknowledgement");
    owner.ReportVrcamOff(owner.OffRequestToken(),63,true);scene.ordinal=2;d=owner.OnSceneFrame(scene);
    check(d.active,"new world starts only after confirmed off and new FrameBegin");
    auto old=scene;old.worldEpoch=1;old.ordinal=999;d=owner.OnSceneFrame(old);
    check(!d.active && owner.Epochs().gameWorld==2,"late previous-world callback cannot roll owner back");
    runtime.enabled=false;owner.UpdateRuntime(runtime);scene.ordinal=3;d=owner.OnSceneFrame(scene);
    check(!d.active && owner.OffRequestToken()==0 && d.invalidate,"author-mode transition releases AER camera ownership");
    std::printf("PASS %u AER scene lifecycle checks\n",checks);
}

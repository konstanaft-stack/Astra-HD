#include "Runtimes/EditorNativeUiPacketPolicy.hpp"
#include <cstdio>
using namespace cvr::menu_preview;
namespace cvr::menu_preview {bool EditorFreshnessEnabled()noexcept{return false;}void NoteEditorPublication(const CameraApplication*,const CameraApplication*,bool,bool)noexcept{}}
int main(){
 const auto now=PublicationNowMs();Metadata common{};common.screen=2;common.screenEpoch=1;common.setupID=6;common.controllerIdentity=10;common.entityIdentity=20;common.entityID=21;common.componentIdentity=30;common.virtualCameraNameHash=40;common.publishedAtMs=now;
 NativePreviewSource l{30,40,0,NativePreviewMode::NativeEditorDual,3},r{31,41,1,NativePreviewMode::NativeEditorDual,3};ConfigureEditorNativeApplications(common,3,l,r);
 auto app=std::make_shared<CameraApplication>();app->metadata=common;app->source=l;app->frame={{1,6,30,40},100,0,10,true};app->nativeContext=1000;app->localPoseRevision=10;app->editorControlRevision=7;app->projection.valid=true;app->projection.origin=9;
 UiFrameReceipt receipt;receipt.application=app;receipt.scope=common;receipt.eye=0;receipt.nativeFrame=100;receipt.sourceSequence=10;receipt.cameraRevision=10;receipt.projection=app->projection;receipt.uiSubmission=20;receipt.queue=500;receipt.backBuffer=600;
 PublishEditorNativeApplication(app);auto next=std::make_shared<CameraApplication>(*app);next->frame.nativeFrame=101;next->frame.sourceSequence=11;next->localPoseRevision=11;PublishEditorNativeApplication(next);
 unsigned failures{};auto check=[&](bool actual,const char* why){if(!actual){++failures;std::fprintf(stderr,"FAIL %s\n",why);}};
 check(!IsCurrentCameraApplication(receipt.application)&&receipt.application==app,"fixture must hold actual receiptN after appNplus1");
 const auto proven=[&](const UiFrameReceipt& value,uint64_t controls=7,uint64_t origin=9,uint64_t generation=3){return cvr::xr::EditorNativeUiReceiptProven(value,common,controls,origin,generation,now);};
 check(proven(receipt),"actual UI receiptN must remain usable after camera appNplus1");
 auto copy=std::make_shared<UiFrameReceipt>(receipt);receipt={};check(proven(*copy),"immutable copied UI receipt lost actual application provenance");
 for(unsigned n=0;n<9;++n){auto wrong=*copy;switch(n){case 0:wrong.application.reset();break;case 1:wrong.eye=1;break;case 2:wrong.nativeFrame++;break;case 3:wrong.sourceSequence++;break;case 4:wrong.cameraRevision++;break;case 5:wrong.scope.setupID++;break;case 6:wrong.uiSubmission=0;break;case 7:wrong.projection.pose.position.x=1;break;case 8:wrong.application=next;break;}
  check(!proven(wrong),"UI receipt was relabelled or missing actual producer proof");}
 check(!proven(*copy,8)&&!proven(*copy,7,10)&&!proven(*copy,7,9,4),"body pair controls/origin/generation mismatch accepted");
 check(!cvr::xr::EditorNativeUiReceiptProven(*copy,common,7,9,3,now+501),"stale actual UI receipt admitted");
 auto right=std::make_shared<CameraApplication>(*app);right->source=r;right->frame.eye=1;auto fakeRight=*copy;fakeRight.application=right;fakeRight.eye=1;
 check(!proven(fakeRight),"offscreen right source was invented as primary UI receipt");
 ClearCameraApplication();check(!proven(*copy),"authoritative clear left prior UI source registered");
 std::printf("actual native UI provenance: %u failures; receiptN retained after appNplus1, exact source/scope/frame/controls/origin/gen and no right UI\n",failures);return failures?1:0;
}

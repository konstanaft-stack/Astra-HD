#include "MenuPreview/ImageLineagePolicy.hpp"
#include <cassert>
#include <iostream>
using namespace cvr::menu_preview;
static std::shared_ptr<CameraApplication> App(uint32_t epoch=1,uint32_t frame=20){
    auto a=std::make_shared<CameraApplication>();
    a->metadata.screen=1;a->metadata.screenEpoch=epoch;a->metadata.setupID=2;
    a->metadata.componentIdentity=0x111;a->metadata.virtualCameraNameHash=0x222;a->metadata.publishedAtMs=100;
    a->frame.key={epoch,2,0x111,0x222};a->frame.nativeFrame=frame;a->frame.eye=1;a->frame.sourceSequence=8;
    a->nativeContext=0x333;return a;
}
int main(){
    auto a=App();
    assert(MatchPreviewNode(a,0x333,0x222,true,20,101));
    auto editor=App();editor->metadata.screen=2;editor->metadata.setupID=6;editor->frame.key.setup=6;
    assert(MatchPreviewNode(editor,0x333,0x222,true,20,101));editor->metadata.screen=3;
    assert(!MatchPreviewNode(editor,0x333,0x222,true,20,101));
    assert(!MatchPreviewNode(a,0x334,0x222,true,20,101));
    assert(!MatchPreviewNode(a,0x333,0x223,true,20,101));
    assert(!MatchPreviewNode(a,0x333,0x222,true,19,101));
    assert(!MatchPreviewNode(a,0x333,0x222,false,20,101));
    assert(!MatchPreviewNode(a,0x333,0x222,true,20,601));
    auto timed=App();timed->metadata.publishedAtMs=PublicationNowMs();
    assert(MatchPreviewNode(timed,0x333,0x222,true,20,PublicationNowMs()));
    assert(!MatchPreviewNode(timed,0x333,0x222,true,20,timed->metadata.publishedAtMs+1000000));
    ImageLineageStore s;const uintptr_t ls[]{10};
    s.ObserveScope(a);s.Record(10,1000,a);s.Record(10,1000,a);
    auto p=s.Prepare(5,std::span(ls));s.Reset(10);
    auto b=App(1,21);b->frame.sourceSequence=9;s.Record(10,1000,b);
    s.Commit(5,std::move(p),102);
    auto image=s.Read(1000);assert(image&&image.pose==a&&image.generation==1&&image.queue==5);
    assert(s.Validate(image,5));assert(!s.Validate(image,6));
    // Reset after Prepare cannot rewrite the submitted batch's receipt.
    s.Record(10,1000,b);
    auto later=s.Prepare(5,std::span(ls));s.Commit(5,std::move(later),103);
    assert(s.Read(1000).pose==b);
    assert(!s.Validate(image,5));
    // An unknown bind on the same resource revokes the prior label.
    s.Record(10,1000,{});auto unknown=s.Prepare(5,std::span(ls));s.Commit(5,std::move(unknown),104);
    assert(!s.Read(1000));
    s.Record(10,1000,a);auto delayed=s.Prepare(5,std::span(ls));s.ObserveScope(App(2));
    s.Commit(5,std::move(delayed),105);assert(!s.Read(1000));
    // Even a newly prepared old recording cannot cross a screen epoch.
    auto stale=s.Prepare(5,std::span(ls));s.Commit(5,std::move(stale),106);assert(!s.Read(1000));
    s.ObserveScope(a);s.Record(10,1000,a);auto resourceGone=s.Prepare(5,std::span(ls));s.ForgetResource(1000);
    s.Record(10,1000,b);s.Commit(5,std::move(resourceGone),107);assert(!s.Read(1000));
    auto listGone=s.Prepare(5,std::span(ls));s.ForgetList(10);s.Record(10,1000,a);
    s.Commit(5,std::move(listGone),108);assert(!s.Read(1000));
    auto wrongQueue=s.Prepare(5,std::span(ls));s.Commit(6,std::move(wrongQueue),109);assert(!s.Read(1000));
    s.Record(10,1000,a);auto final=s.Prepare(5,std::span(ls));s.Commit(5,std::move(final),110);
    assert(s.Read(1000));s.ObserveScope({});assert(!s.Read(1000));
    // Nested dispatch can clear a parent's receipt then restore it, never leak.
    NodeBindingStack stack;auto outer=stack.Enter(a,0x377B58);assert(stack.Current()==a&&stack.WorkRva()==0x377B58);
    auto inner=stack.Enter({},0x123);assert(!stack.Current()&&stack.WorkRva()==0x123);stack.Restore(inner);
    assert(stack.Current()==a&&stack.WorkRva()==0x377B58);stack.Restore(outer);assert(!stack.Current()&&stack.WorkRva()==0);
    ImageLineageStore workStore;workStore.ObserveScope(a);
    RtvBindingPayload copy{a};copy.workRva=0x377B58;copy.width=720;copy.height=720;copy.format=29;copy.descriptorKnown=true;
    workStore.RecordBinding(10,1000,copy);auto preparedWork=workStore.Prepare(5,std::span(ls));
    auto replacement=copy;replacement.workRva=0x123;replacement.width=1920;
    workStore.Reset(10);workStore.RecordBinding(10,1000,replacement);workStore.Commit(5,std::move(preparedWork),111);
    const auto submittedWork=workStore.Read(1000);
    assert(submittedWork&&submittedWork.pose.workRva==0x377B58&&submittedWork.pose.width==720&&submittedWork.pose.descriptorKnown);
    std::cout<<"MenuPreview ImageLineage policy PASS: exact node, immutable submit, unknown overwrite, epoch, resource/list reuse, queue, nesting; RTV bind only\n";
}

#include "MenuPreview/UiQualifiedSourcePolicy.hpp"
#include <array>
#include <memory>
#include <cstdio>
#include <cstdlib>
using namespace cvr::menu_preview;
void require(bool v,const char* why){if(!v){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}}
struct Scope{unsigned epoch{},setup{};bool operator==(const Scope&)const=default;};
struct Owner{bool* held;unsigned* destroyed;unsigned* underLock;~Owner(){++*destroyed;if(*held)++*underLock;}};
struct Target{Scope sourceScope{},editorScope{};uintptr_t resource{},source{},queue{},qualifiedQueue{},editorList{};uint64_t generation{},sourceGeneration{},submission{},qualifiedSubmission{},editorSubmission{},sourceSequence{},handleCursor{};bool editorUiRequired{};std::array<uint64_t,4> handles{};std::shared_ptr<Owner> qualifiedApp,editorUi;};
int main(){
    bool held=false;unsigned appDestroyed=0,uiDestroyed=0,underLock=0;const Scope lips{1,9};Target target;
    target.resource=100;target.generation=50;target.source=200;target.sourceGeneration=1;target.sourceScope=lips;
    target.editorScope=lips;target.queue=target.qualifiedQueue=900;target.submission=target.qualifiedSubmission=target.editorSubmission=7;target.sourceSequence=11;target.editorList=30;target.editorUiRequired=true;target.handles={1,2,3,4};target.handleCursor=4;
    target.qualifiedApp=std::shared_ptr<Owner>(new Owner{&held,&appDestroyed,&underLock});target.editorUi=std::shared_ptr<Owner>(new Owner{&held,&uiDestroyed,&underLock});
    auto actualGpuReader=target.editorUi;std::weak_ptr<Owner> oldApp=target.qualifiedApp,oldUi=target.editorUi;
    UiRetiredQualifiedOwners<std::shared_ptr<Owner>,std::shared_ptr<Owner>> retired;
    held=true;
    require(DecideUiQualifiedSource(true,target.sourceScope==lips,target.source,target.sourceGeneration,201,2)==UiQualifiedSourceAction::Replace,"a verified new primary source B replaces transient A on the same category UI target");
    retired=ReplaceUiQualifiedTargetSource(target,201,2,lips);
    require(target.resource==100&&target.generation==50&&target.source==201&&target.sourceGeneration==2&&target.sourceScope==lips,"target resource lifetime stays exact while source identity changes");
    require(!target.qualifiedApp&&!target.editorUi&&!target.submission&&!target.qualifiedSubmission&&!target.editorSubmission&&!target.queue&&!target.qualifiedQueue&&!target.editorList&&!target.sourceSequence&&!target.handleCursor&&!target.editorUiRequired&&target.editorScope==Scope{}&&target.handles==std::array<uint64_t,4>{},"new source inherits no old content, queue, canvas, scope or handle proof");
    require(!oldApp.expired()&&!oldUi.expired()&&!appDestroyed&&!uiDestroyed,"proof owners remain alive through the observer lock");
    require(!UiQualifiedSourceEventMatches(target.source,target.sourceGeneration,200,1)&&UiQualifiedSourceEventMatches(target.source,target.sourceGeneration,201,2),"a frozen old event cannot qualify replacement B or restore relation A; actual writes still revoke content proof");
    held=false;retired={};require(oldApp.expired()&&appDestroyed==1&&!oldUi.expired()&&!underLock,"retired owners release outside lock and real GPU reader independently retains its canvas");
    actualGpuReader.reset();require(oldUi.expired()&&uiDestroyed==1&&!underLock,"canvas dies only after actual reader legitimately releases");
    for(unsigned n=0;n<1000;++n)require(DecideUiQualifiedSource(true,true,target.source,target.sourceGeneration,201,2)==UiQualifiedSourceAction::Keep,"sustained B and repeated verified draws retain its current source identity");
    require(DecideUiQualifiedSource(false,true,200,1,201,2)==UiQualifiedSourceAction::Reject&&DecideUiQualifiedSource(false,true,200,1,200,1)==UiQualifiedSourceAction::Keep,"legacy still rejects source changes and accepts exact repeats");
    require(DecideUiQualifiedSource(true,false,201,2,202,3)==UiQualifiedSourceAction::Reject,"cross-category or owner scope cannot inherit relation");
    require(DecideUiQualifiedSource(true,true,201,2,0,3)==UiQualifiedSourceAction::Reject&&DecideUiQualifiedSource(true,true,201,2,202,0)==UiQualifiedSourceAction::Reject,"unknown resource or generation fails closed");
    require(DecideUiQualifiedSource(true,true,201,2,200,3)==UiQualifiedSourceAction::Replace,"ABA address with a new generation is a fresh replacement");
    auto again=ReplaceUiQualifiedTargetSource(target,200,3,lips);require(!again.app&&!again.ui&&!UiQualifiedSourceEventMatches(target.source,target.sourceGeneration,200,1),"old A generation cannot return through an ABA address");
    require(DecideUiQualifiedSource(true,true,200,3,200,4)==UiQualifiedSourceAction::Replace,"same address with changed object generation is never an exact repeat");
    std::puts("PASS transient A-to-B UI target rebind, sustained repeats, full proof reset, immutable old events, independent owner retirement, ABA and legacy/scope rejection");
}

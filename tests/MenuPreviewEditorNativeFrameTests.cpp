#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

#if __has_include("MenuPreview/EditorNativeFrame.hpp")
#include "MenuPreview/EditorNativeFrame.hpp"
using namespace cvr::menu_preview;
namespace {
void require(bool value,const char* message){if(!value){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
Metadata scope(){Metadata m{};m.screen=2;m.screenEpoch=3;m.setupID=6;m.componentIdentity=0x100;m.virtualCameraNameHash=0x200;return m;}
XrPosef pose(float x=0){return {{0,0,0,1},{x,1.6f,-.2f}};}
bool samePose(const XrPosef& a,const XrPosef& b){return a.orientation.x==b.orientation.x&&a.orientation.y==b.orientation.y&&a.orientation.z==b.orientation.z&&a.orientation.w==b.orientation.w&&a.position.x==b.position.x&&a.position.y==b.position.y&&a.position.z==b.position.z;}
void pairedSample(){
    EditorNativeFrameLatch latch;const auto m=scope();const auto head=pose(1);
    const auto left=latch.Resolve(m,11,100,0,7,head);
    require(left&&left.frame.eye==0&&left.frame.changed&&left.firstInFrame,"left first native eye starts a common frame");
    require(left.frame.key==PreviewFrameKey{3,6,0x100,0x200}&&left.bindingGeneration==11&&left.origin==7,"eye identity uses the canonical primary metadata and exact binding generation");
    const auto right=latch.Resolve(m,11,100,1,7,pose(2));
    require(right&&right.frame.eye==1&&right.frame.changed&&!right.firstInFrame,"second actual native eye is allowed in the same frame");
    require(right.frame.sourceSequence>left.frame.sourceSequence,"the two actual eye sources have distinct monotonic sequences");
    require(samePose(right.headPose,head)&&right.origin==left.origin&&right.frame.key==left.frame.key,"both native eyes use the first frame's frozen head and origin");
    const auto again=latch.Resolve(m,11,100,0,7,pose(3));
    require(again&&!again.frame.changed&&!again.firstInFrame&&again.frame.sourceSequence==left.frame.sourceSequence&&samePose(again.headPose,head),"same-eye repeats retain sequence and frozen snapshot");
    const auto next=latch.Resolve(m,11,101,1,8,pose(4));
    require(next&&next.firstInFrame&&next.frame.changed&&next.frame.sourceSequence>right.frame.sourceSequence&&samePose(next.headPose,pose(4))&&next.origin==8,"a new frame refreshes its common snapshot, origin and source sequence");
    require(!latch.Resolve(m,11,100,1,7,head),"older native work cannot reuse the previous frame's eye");
    std::puts("PASS same-frame dual eyes, frozen head/origin, repeats and new-frame refresh");
}
void rightFirst(){
    EditorNativeFrameLatch latch;const auto m=scope();const auto right=latch.Resolve(m,1,10,1,3,pose(5));
    const auto left=latch.Resolve(m,1,10,0,3,pose(6));
    require(right&&right.frame.eye==1&&right.firstInFrame&&left&&left.frame.eye==0&&!left.firstInFrame,"native right-first execution preserves both actual fixed eyes");
    require(left.frame.sourceSequence>right.frame.sourceSequence&&samePose(left.headPose,right.headPose),"right-first pair shares one snapshot while source sequences stay ordered");
    std::puts("PASS right-first native order");
}
void validation(){
    const auto m=scope();const auto p=pose();EditorNativeFrameLatch latch;
    require(!latch.Resolve(m,0,10,0,1,p)&&!latch.Resolve(m,1,10,2,1,p)&&!latch.Resolve(m,1,10,0,0,p),"unknown binding, eye or origin cannot establish a frame");
    auto bad=m;bad.screen=1;require(!latch.Resolve(bad,1,10,0,1,p),"inventory cannot establish an editor frame");
    bad=m;bad.screenEpoch=0;require(!latch.Resolve(bad,1,10,0,1,p),"zero editor epoch rejected");
    bad=m;bad.componentIdentity=0;require(!latch.Resolve(bad,1,10,0,1,p),"missing canonical component rejected");
    bad=m;bad.virtualCameraNameHash=0;require(!latch.Resolve(bad,1,10,0,1,p),"missing canonical camera rejected");
    auto corrupt=p;corrupt.position.z=std::numeric_limits<float>::quiet_NaN();
    require(!latch.Resolve(m,1,10,0,1,corrupt),"nonfinite position cannot latch a frame");
    corrupt=p;corrupt.orientation.x=std::numeric_limits<float>::infinity();
    require(!latch.Resolve(m,1,10,0,1,corrupt),"nonfinite orientation cannot latch a frame");
    corrupt=p;corrupt.orientation={0,0,0,0};require(!latch.Resolve(m,1,10,0,1,corrupt),"degenerate orientation cannot latch a frame");
    const auto accepted=latch.Resolve(m,1,10,0,1,p);require(accepted&&accepted.frame.sourceSequence==1,"invalid attempts do not consume source sequences or common snapshots");
    corrupt=p;corrupt.position.x=std::numeric_limits<float>::quiet_NaN();
    require(!latch.Resolve(m,1,10,0,1,corrupt),"invalid current pose is rejected even for a cached eye");
    require(!latch.Resolve(m,1,10,1,2,p),"mid-frame tracking-origin changes cannot pair two different origins");
    const auto second=latch.Resolve(m,1,10,1,1,p);require(second&&second.frame.sourceSequence==2,"rejected origin/pose attempts leave the second source unconsumed");
    std::puts("PASS invalid inputs and non-mutating rejection");
}
void keysAndClock(){
    auto m=scope();EditorNativeFrameLatch latch;auto a=latch.Resolve(m,2,50,0,1,pose());require(bool(a),"clock test starts");
    for(unsigned field=0;field<5;++field){
        auto changed=m;uint64_t generation=2;
        if(field==0)++changed.screenEpoch;if(field==1)++changed.setupID;if(field==2)++changed.componentIdentity;if(field==3)++changed.virtualCameraNameHash;if(field==4)++generation;
        require(!latch.Resolve(changed,generation,a.frame.nativeFrame,1,1,pose()),"mid-frame scope or binding mutation cannot join a partial eye pair");
        require(!latch.Resolve(changed,generation,a.frame.nativeFrame-1,1,1,pose()),"identity changes cannot bypass the old-frame watermark");
        const auto fresh=latch.Resolve(changed,generation,a.frame.nativeFrame+1,1,1,pose(float(field)));
        require(fresh&&fresh.firstInFrame&&fresh.frame.sourceSequence>a.frame.sourceSequence,"every key field refreshes a subsequent native frame without recycling sequences");
        a=fresh;m=changed;
        // Restore the original generation for the next deliberate change.
        if(field==4)break;
    }
    EditorNativeFrameLatch wrap;
    const auto high=wrap.Resolve(scope(),1,0xfffffffeu,0,1,pose());
    const auto top=wrap.Resolve(scope(),1,0xffffffffu,1,1,pose());
    const auto zero=wrap.Resolve(scope(),1,0,0,1,pose());
    require(high&&top&&zero&&zero.frame.sourceSequence>top.frame.sourceSequence,"native uint32 wrap advances through zero");
    require(!wrap.Resolve(scope(),1,0xffffffffu,1,1,pose()),"pre-wrap work is older than the post-wrap frame");
    require(!wrap.Resolve(scope(),1,0x80000000u,1,1,pose()),"half-range ambiguous native clock jump fails closed");
    require(bool(wrap.Resolve(scope(),1,1,1,1,pose())),"a rejected clock jump does not advance the watermark");
    std::puts("PASS canonical key/binding generation and uint32 native clock wrap");
}
void clearAndOverflow(){
    const auto m=scope();EditorNativeFrameLatch latch;const auto first=latch.Resolve(m,1,90,0,1,pose());
    latch.Clear();const auto after=latch.Resolve(m,1,1,1,2,pose(2));
    require(first&&after&&after.firstInFrame&&after.frame.sourceSequence>first.frame.sourceSequence&&samePose(after.headPose,pose(2)),"Clear discards cached clock/snapshot but keeps source sequences monotonic");
    EditorNativeFrameLatch nearlyFull(std::numeric_limits<uint64_t>::max()-1);
    const auto last=nearlyFull.Resolve(m,1,8,1,1,pose());require(last&&last.frame.sourceSequence==std::numeric_limits<uint64_t>::max(),"the final nonzero source sequence remains usable");
    require(!nearlyFull.Resolve(m,1,8,0,1,pose()),"second eye cannot wrap the source counter to zero");
    const auto repeat=nearlyFull.Resolve(m,1,8,1,1,pose(4));require(repeat&&!repeat.frame.changed&&repeat.frame.sourceSequence==last.frame.sourceSequence&&samePose(repeat.headPose,pose()),"overflow rejection preserves an already owned eye's immutable snapshot");
    require(!nearlyFull.Resolve(m,1,9,1,1,pose(9)),"a new frame cannot bypass source-counter exhaustion");
    nearlyFull.Clear();require(!nearlyFull.Resolve(m,1,1,0,1,pose()),"Clear cannot reset an exhausted monotonic counter");
    std::puts("PASS Clear and source-counter exhaustion");
}
}
int main(){pairedSample();rightFirst();validation();keysAndClock();clearAndOverflow();std::puts("PASS editor native frame latch contract; this is CPU identity/snapshot evidence only");}
#else
// Behavior RED against the actual old owner: its second same-frame callback
// cannot represent the other actual native eye. This does not test the future
// latch's head-pose validation, and is not a game/rendering reproduction.
#include "MenuPreview/SpatialEditorPolicy.hpp"
int main(){
    using namespace cvr::menu_preview;Metadata m{};m.screen=2;m.screenEpoch=3;m.setupID=6;m.componentIdentity=0x100;m.virtualCameraNameHash=0x200;
    EditorCaptureOwner old;const auto left=old.Resolve(m,100,false,0);const auto actualRight=old.Resolve(m,100,true,0);
    if(!left||left.eye!=0||!actualRight||actualRight.eye!=1||actualRight.sourceSequence==left.sourceSequence){
        std::fprintf(stderr,"FAIL: old EditorCaptureOwner cannot own both actual native eyes in one frame (left=%u right=%u sameSequence=%d)\n",left.eye,actualRight.eye,int(actualRight.sourceSequence==left.sourceSequence));return 1;
    }
    return 0;
}
#endif

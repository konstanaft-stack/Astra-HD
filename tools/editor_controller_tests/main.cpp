#include "MenuPreview/EditorControllerPolicy.hpp"
#include "MenuPreview/UiPairPolicy.hpp"
#include <cstdio>
#include <limits>
using namespace cvr::menu_preview;
namespace {
unsigned failures{};
void Check(bool value,const char* name){if(!value){std::fprintf(stderr,"FAIL %s\n",name);++failures;}}
bool Near(float a,float b){return std::abs(a-b)<.0002f;}
EditorControllerSample Sample(int64_t t=1000000000){return {t,1,1,0,1,.2f,true,false,false};}
float Integrate(unsigned hz){EditorControllerPolicy p;auto s=Sample();p.Update(s);float height{};
 for(unsigned n=1;n<=hz;++n){s.timeNs=1000000000+int64_t(1000000000ull*n/hz);height+=p.Update(s).height;}return height;}
}
int main(){
 EditorControllerPolicy p;auto s=Sample();s.grip=.6f;
 Check(!p.Update(s).claimed,"unpressed grip passes normal input");
 s.grip=.8f;auto step=p.Update(s);Check(step.claimed&&Near(step.height,0),"grip claims editor axes without initial jump");
 s.timeNs+=20000000;step=p.Update(s);Check(step.claimed&&step.height>0&&Near(step.distance,0),"up raises camera");
 s.timeNs+=20000000;s.grip=.6f;Check(p.Update(s).claimed,"grip hysteresis stays held");
 s.timeNs+=20000000;s.grip=.5f;Check(!p.Update(s).claimed,"grip release restores ordinary input");
 s=Sample();p.Reset();p.Update(s);s.timeNs+=20000000;s.stickY=-1;Check(p.Update(s).height<0,"down lowers camera");
 s.timeNs+=20000000;s.stickY=0;s.stickX=1;Check(p.Update(s).distance>0,"right moves camera closer");
 s.timeNs+=20000000;s.stickX=-1;Check(p.Update(s).distance<0,"left moves camera away");
 s.timeNs+=20000000;s.stickX=s.stickY=.1f;step=p.Update(s);Check(step.claimed&&Near(step.height,0)&&Near(step.distance,0),"neutral deadzone consumes without drifting");
 s.timeNs+=20000000;s.stickX=1;s.stickClick=true;step=p.Update(s);Check(step.reset&&Near(step.distance,0),"stick click resets instead of moving");
 s.timeNs+=20000000;step=p.Update(s);Check(!step.reset&&Near(step.distance,0),"held reset does not repeat or move");
 s.timeNs+=20000000;s.stickClick=false;s.available=false;Check(!p.Update(s).claimed,"inactive actions release capture");
 s.available=true;s.timeNs+=20000000;Check(Near(p.Update(s).distance,0),"reactivation has no catch-up jump");
 s.timeNs+=500000000;Check(Near(p.Update(s).distance,0),"large frame gap has no accumulated jump");
 s.timeNs+=20000000;s.origin=2;Check(Near(p.Update(s).distance,0),"recenter restarts time integration");
 s.timeNs+=20000000;s.blocked=true;Check(!p.Update(s).claimed,"port overlay keeps its own controls");
 s.blocked=false;s.timeNs+=20000000;s.stickX=std::numeric_limits<float>::quiet_NaN();Check(!p.Update(s).claimed,"nonfinite sample cannot move camera");
 Check(Near(Integrate(60),Integrate(120))&&Integrate(60)>.1f,"speed is independent of XR refresh rate");

 EditorControlLatch latch;uint64_t counter{};
 auto left=latch.Resolve(1,0,0,0,0,counter);Check(left.revision!=0,"first controls revision is nonzero");
 auto right=latch.Resolve(2,1,1,1,0,counter);Check(right.revision==left.revision&&Near(right.height,0),"right eye never uses a newer pending control position");
 auto retry=latch.Resolve(3,0,1,1,0,counter);Check(retry.revision==left.revision&&Near(retry.height,0),"unpresented pair freezes both controls through left retries");
 auto next=latch.Resolve(4,0,1,1,left.revision,counter);Check(next.revision>left.revision&&Near(next.height,1),"new controls start after actual pair acceptance on left eye");
 auto repeated=latch.Resolve(4,0,2,2,next.revision,counter);Check(repeated.revision==next.revision&&Near(repeated.height,1),"same source cannot change its camera");
 right=latch.Resolve(5,1,2,2,next.revision,counter);Check(right.revision==next.revision&&Near(right.height,1),"right eye holds matching offsets even after acceptance");
 const auto old=next.revision;latch.Reset();next=latch.Resolve(6,0,2,2,old,counter);Check(next.revision>old,"new scope cannot reuse an accepted old revision");
 Check(!latch.Resolve(5,0,0,0,next.revision,counter).revision,"older source cannot regress the controls owner");

 UiPairScope scope{1,2,3,4,5,6};UiPairPolicy pair;
 pair.Publish({scope,10,0,1,1,1,0,100,1});pair.Publish({scope,20,0,2,2,2,1,101,1});
 Check(bool(pair.Read(scope)),"matching controller revision produces a stereo pair");
 pair.Publish({scope,30,0,3,3,3,0,102,2});Check(!pair.Read(scope),"mixed old-right and new-left controls are not presented");
 pair.Publish({scope,40,0,4,4,4,1,103,2});Check(bool(pair.Read(scope)),"pair resumes when both eye controls match");
 UiPairPolicy legacy;legacy.Publish({scope,50,0,1,1,1,0,10});legacy.Publish({scope,60,0,2,2,2,1,11});Check(bool(legacy.Read(scope)),"legacy and inventory zero revision remains supported");
 if(failures)return 1;std::puts("PASS left grip/stick semantics, timing/reset, stereo control latch and mixed-pair rejection");return 0;
}

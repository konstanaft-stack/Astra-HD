#include "Stereo/NativeSlFrameTimeline.hpp"
#include <cstdlib>
#include <iostream>
using cvr::stereo::NativeSlFrameTimeline;
void check(bool value,const char* error){if(!value){std::cerr<<error<<'\n';std::exit(1);}}
int main(){
    NativeSlFrameTimeline frames;
    check(!frames.Resolve(1,10),"Unobserved frame was invented");
    frames.Publish(1,10,400);
    check(frames.Resolve(1,10)==400u,"Renderer clock was substituted for SL clock");
    check(!frames.Resolve(1,11),"Missing frame was extrapolated");
    frames.Publish(1,11,900);
    check(frames.Resolve(1,11)==900u,"Observed jump must be preserved, not inferred");
    frames.Publish(1,11,900);
    check(frames.Resolve(1,11)==900u,"Duplicate identical observation corrupted stamp");
    frames.Publish(1,11,901);
    check(!frames.Resolve(1,11),"Ambiguous stamp was silently accepted");
    frames.Publish(1,11,900);
    check(!frames.Resolve(1,11),"Subsequent sample erased conflict");
    check(!frames.Resolve(2,10),"Renderer owner mismatch was accepted");
    frames.Publish(2,10,0);
    check(frames.Resolve(2,10)==0u,"Explicitly observed zero was mistaken for unknown");
    check(!frames.Resolve(1,10),"Previous renderer generation remained accessible");
    frames.Publish(0,10,401);
    check(!frames.Resolve(0,10),"Null renderer accepted");
    check(frames.Resolve(2,10)==0u,"Invalid observation replaced active renderer");
    frames.Publish(2,18,300);
    check(!frames.Resolve(2,10),"Reused ring cell returned stale frame");
    frames.Publish(2,0xffffffffu,4500);
    frames.Publish(2,0,4501);
    check(frames.Resolve(2,0)==4501u,"Counter wrap used arithmetic rather than observation");
    std::cout<<"PASS: observed indices, ambiguity, owner reset, null, ring reuse, wrap\n";
}

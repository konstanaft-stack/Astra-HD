#include "Stereo/VrcamResizeState.hpp"
#include <cstdio>

using namespace cvr::ppd;
static int failures=0,checks=0;
static void check(bool ok,const char* label) {
    ++checks;
    if(!ok){std::fprintf(stderr,"FAIL: %s\n",label);++failures;}
}
static VrcamResizeLayout layout(uint32_t width=1920,uint32_t height=2080) {
    return {{width,height},{width,height},{{1840,2000},{1856,2016}}};
}
static uint64_t request(VrcamResizeState& state,uint32_t width=1920,uint32_t height=2080) {
    state.UpdateWorld(9);
    return state.Request(layout(width,height),9,0x1234,20,100);
}
static VrcamResizeObservation actual(uint64_t id) {
    return {id,9,0x1234,21,101,{1920,2080},{1920,2080},{{1840,2000},{1856,2016}},3,true};
}
static void dispatch(VrcamResizeState& state,uint64_t id) {
    const auto work=state.BeginDispatch(0x1234);
    check(work.id==id && id!=0,"driver receives current request only");
    state.MarkDispatched(id);
}
struct ResourceFixture {
    uint32_t authoredWidth=2560,authoredHeight=2560,viewportWidth=2560,viewportHeight=2560,calls{};
};
static bool viewportApi(void* resource,uint32_t width,uint32_t height) {
    auto& r=*static_cast<ResourceFixture*>(resource);
    r.viewportWidth=width;r.viewportHeight=height;++r.calls;
    return true; // checked dispatch only; actual resource/presentation arrives later
}
static bool failingViewportApi(void*,uint32_t,uint32_t) {return false;}
static bool sceneChangeViewportApi(void* resource,uint32_t,uint32_t) {
    static_cast<VrcamResizeState*>(resource)->UpdateWorld(10);
    return true;
}

int main() {
    {
        VrcamResizeState state;
        check(state.Request(layout(),9,0x1234,20,100)==0,"unknown world rejects request");
        state.UpdateWorld(9);
        check(state.Request(layout(),8,0x1234,20,100)==0,"stale world rejects request");
        check(state.Request(layout(1928,2080),9,0x1234,20,100)==0,"new resize requires 16 alignment");
        check(state.Request(layout(0,2080),9,0x1234,20,100)==0,"zero dimension rejected");
        check(state.Request(layout(16400,2080),9,0x1234,20,100)==0,"oversized texture rejected");
        check(state.Request(layout(),9,0,20,100)==0,"unknown bound camera rejected");
        check(state.Request(layout(),9,0x1234,0,100)==0,"unknown baseline resource epoch rejected");
    }
    {
        VrcamResizeState state;const auto id=request(state);
        check(id!=0 && state.Read().phase==VrcamResizePhase::Requested,"request alone is pending");
        check(!state.ObserveActual(actual(id)),"descriptor before native dispatch cannot apply");
        check(state.BeginDispatch(0x7777).id==0,"unrelated view cannot consume request");
        dispatch(state,id);
        check(state.Read().phase==VrcamResizePhase::Submitted,"native void return means submitted only");
        check(state.BeginDispatch(0x1234).id==0,"one native dispatch per request");
        auto sample=actual(id);sample.producerReady=false;
        check(!state.ObserveActual(sample),"pending producer fence cannot apply");
        sample=actual(id);sample.acceptedEyeMask=1;
        check(!state.ObserveActual(sample),"one accepted eye cannot apply");
        check(state.Read().phase==VrcamResizePhase::ReadyForPresentation,"matched source and eye resources are ready, not Applied");
        sample=actual(id);sample.acceptedEyeMask=2;sample.sourceSerial=102;
        check(!state.ObserveActual(sample),"different draws cannot accumulate an accepted pair");
        sample=actual(id);sample.sourceSerial=100;
        check(!state.ObserveActual(sample),"old source serial cannot apply");
        sample=actual(id);sample.resourceEpoch=20;
        check(!state.ObserveActual(sample),"old resource epoch cannot apply");
        sample=actual(id);sample.vrcamSource={1904,2080};
        check(!state.ObserveActual(sample),"old VRCAM descriptor cannot apply");
        sample=actual(id);sample.mainSource={1920,2064};
        check(!state.ObserveActual(sample),"old MAIN descriptor cannot apply");
        check(state.Read().phase==VrcamResizePhase::EngineApplied,"VRCAM descriptor alone is only engine-applied");
        sample=actual(id);sample.eyes[1]={1920,2064};
        check(!state.ObserveActual(sample),"old output eye descriptor cannot apply");
        sample=actual(id);sample.cameraKey=0x7777;
        check(!state.ObserveActual(sample),"different camera cannot acknowledge");
        check(state.ObserveActual(actual(id)),"matching new ready source and both accepted eyes apply");
        const auto snapshot=state.Read();
        check(snapshot.phase==VrcamResizePhase::Applied && snapshot.appliedResourceEpoch==21 &&
              snapshot.appliedSourceSerial==101,"applied state retains actual epoch and serial");
    }
    {
        VrcamResizeState state;const auto old=request(state);dispatch(state,old);
        const auto current=state.Request(layout(2048,2112),9,0x1234,21,102);
        check(current!=0 && current!=old,"new request replaces bounded pending state");
        check(!state.MarkDispatched(old),"stale completion cannot mark new request submitted");
        check(!state.ObserveActual(actual(old)),"stale accepted draw cannot apply new request");
        check(state.Read().phase==VrcamResizePhase::Requested,"stale completion leaves new request pending");
    }
    {
        VrcamResizeState state;const auto id=request(state);dispatch(state,id);
        state.UpdateWorld(10);
        check(state.Read().phase==VrcamResizePhase::Idle,"new world invalidates request and applied state");
        check(!state.ObserveActual(actual(id)) && state.BeginDispatch(0x1234).id==0,"old-world work discarded");
        state.UpdateWorld(0);
        check(state.Request(layout(),10,0x1234,21,102)==0,"unbound world remains unavailable");
    }
    {
        VrcamResizeState state;const auto id=request(state);
        check(state.Fail(id,VrcamResizeError::UnsupportedResource),"driver failure is recorded");
        check(state.Read().phase==VrcamResizePhase::Error &&
              state.Read().error==VrcamResizeError::UnsupportedResource,"failure never acknowledges Applied");
        check(state.BeginDispatch(0x1234).id==0 && !state.ObserveActual(actual(id)),"failed request does not dispatch or apply");
    }
    {
        VrcamResizeState state;request(state);ResourceFixture resource;
        check(!DispatchVrcamResize(state,{0x7777,&resource,true,true},viewportApi),"unrelated RTT view never resizes");
        check(!DispatchVrcamResize(state,{0x1234,nullptr,true,true},viewportApi),"unloaded resource stays pending");
        check(state.Read().phase==VrcamResizePhase::Requested && !resource.calls,"unavailable view does not consume request");
        check(DispatchVrcamResize(state,{0x1234,&resource,true,true},viewportApi),"bound driver submits through viewport API");
        check(resource.calls==1 && resource.viewportWidth==1920 && resource.viewportHeight==2080,
              "native boundary receives VRCAM target, not eye output dimensions");
        check(resource.authoredWidth==2560 && resource.authoredHeight==2560,"authored resource fields stay unchanged");
        check(state.Read().phase==VrcamResizePhase::Submitted,"native setter is never Applied ack");
        check(!DispatchVrcamResize(state,{0x1234,&resource,true,true},viewportApi) && resource.calls==1,
              "driver does not enqueue resize on every frame");
    }
    {
        VrcamResizeState state;request(state);ResourceFixture resource;
        check(!DispatchVrcamResize(state,{0x1234,&resource,false,true},viewportApi),"non-viewport texture rejected");
        check(state.Read().error==VrcamResizeError::UnsupportedResource && !resource.calls,"unsupported texture not written");
    }
    {
        VrcamResizeState state;request(state);ResourceFixture resource;
        check(!DispatchVrcamResize(state,{0x1234,&resource,true,false},viewportApi),"unrecognized executable rejected");
        check(state.Read().error==VrcamResizeError::UnsupportedExecutable && !resource.calls,"unsupported ABI cannot dispatch");
    }
    {
        VrcamResizeState state;request(state);ResourceFixture resource;
        check(!DispatchVrcamResize(state,{0x1234,&resource,true,true},failingViewportApi),"guarded native exception rejects dispatch");
        check(state.Read().error==VrcamResizeError::EngineException,"native exception has explicit error state");
    }
    {
        VrcamResizeState state;const auto id=request(state);
        check(!DispatchVrcamResize(state,{0x1234,&state,true,true},sceneChangeViewportApi),"world invalidation during dispatch prevents submission ack");
        check(state.Read().phase==VrcamResizePhase::Idle && !state.ObserveActual(actual(id)),"scene cancellation survives returning engine call");
    }
    {
        VrcamResizeState state;state.UpdateWorld(9);
        const VrcamResizeLayout different{{1280,1440},{1920,2080},{{1833,1977},{1839,1981}}};
        const auto id=state.Request(different,9,0x1234,20,100);
        dispatch(state,id);
        auto sample=actual(id);sample.mainSource=different.mainSource;
        sample.eyes[0]=different.eyes[0];sample.eyes[1]=different.eyes[1];
        check(state.ObserveActual(sample),"explicit MAIN/VRCAM/asymmetric eye dimensions need not match each other");
    }
    if(failures)return 1;
    std::printf("PASS: %d resize request/native dispatch/actual source and eyes/epoch/scene cancellation checks\n",checks);
}

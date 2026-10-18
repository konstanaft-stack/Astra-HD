#include "Framegen/Inputs.hpp"
#include "Framegen/StreamlineCameraSnapshot.hpp"
#include "Framegen/CaptureCompletion.hpp"
#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

using namespace cvr::framegen;
static void Check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }

static sl::Constants CompleteConstants() {
    sl::Constants c;
    sl::float4x4* matrices[]={&c.cameraViewToClip,&c.clipToCameraView,&c.clipToLensClip,
        &c.clipToPrevClip,&c.prevClipToClip};
    for(unsigned matrix=0;matrix<5;++matrix)for(unsigned row=0;row<4;++row) {
        const float first=float(matrix*20+row*4+1);
        matrices[matrix]->setRow(row,{first,first+1,first+2,first+3});
    }
    c.jitterOffset={.25f,-.125f};c.mvecScale={.5f,-.5f};c.cameraPinholeOffset={.01f,-.02f};
    c.cameraPos={10,20,30};c.cameraUp={0,1,0};c.cameraRight={1,0,0};c.cameraFwd={0,0,-1};
    c.cameraNear=.02f;c.cameraFar=16000;c.cameraFOV=1.8f;c.cameraAspectRatio=1.1f;
    c.motionVectorsInvalidValue=65504;c.minRelativeLinearDepthObjectSeparation=37;
    c.depthInverted=sl::eTrue;c.cameraMotionIncluded=sl::eTrue;c.motionVectors3D=sl::eFalse;
    c.reset=sl::eFalse;c.orthographicProjection=sl::eFalse;
    c.motionVectorsDilated=sl::eTrue;c.motionVectorsJittered=sl::eTrue;
    return c;
}
static void StreamlineFields() {
    auto input=CompleteConstants();const auto saved=SnapshotStreamlineCamera(input);
    // Distinct, hand-ordered rows catch omission, transposition and ABI offset drift.
    const std::array<float,104> expected={
        1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,
        21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,
        41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,
        61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,
        81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,
        .25f,-.125f,.5f,-.5f,.01f,-.02f,
        10,20,30,0,1,0,1,0,0,0,0,-1,.02f,16000,1.8f,1.1f,65504,37};
    Check(saved.values==expected,"Streamline camera values lost or reordered");
    Check(saved.flags==std::array<uint32_t,7>{1,1,0,0,0,1,1},"Streamline Boolean flags lost or reordered");
    Check(saved.source==CameraConstantsSource::Streamline && saved.version==2,"camera provenance lost");
    Check(HasCompleteCameraConstants(saved),"complete measured constants rejected");
    input.clipToPrevClip[0].x=-99;input.cameraPos.x=-100;input.reset=sl::eTrue;
    Check(saved.values[48]==61 && saved.values[86]==10 && saved.flags[3]==0,"snapshot aliases mutable constants");
}
static void VersionedFields() {
    auto input=CompleteConstants();input.structVersion=1;
    const auto v1=SnapshotStreamlineCamera(input);
    Check(v1.valuePresent.count()==103 && !v1.valuePresent[103],"v1 falsely supplies a v2-only field");
    Check(v1.values[102]==65504 && !HasCompleteCameraConstants(v1),"v1 missing-field rejection lost");
    input.structVersion=3;const auto future=SnapshotStreamlineCamera(input);
    Check(future.valuePresent.none() && !HasCompleteCameraConstants(future),"unknown version guessed");
    input=CompleteConstants();input.structType.data1^=1;
    Check(!HasCompleteCameraConstants(SnapshotStreamlineCamera(input)),"foreign SL structure accepted");
}
static void NativeMissing() {
    CameraData c{};c.jitter[0]=.25f;c.jitter[1]=-.125f;c.motionScale[0]=.5f;c.motionScale[1]=-.5f;
    c.position[0]=12;c.position[1]=13;c.position[2]=14;c.up[1]=1;c.right[0]=1;c.forward[2]=-1;
    c.nearPlane=.02f;c.farPlane=16000;c.verticalFov=1.8f;c.aspect=1.1f;
    c.inverted=true;c.cameraMotion=true;c.jittered=true;c.reset=true;
    const auto native=SnapshotNativeCamera(c);
    Check(native.source==CameraConstantsSource::NativeStaging,"native provenance missing");
    Check(native.values[80]==.25f && native.values[86]==12 && native.values[99]==16000,"known native fields lost");
    Check(native.valuePresent.count()==19 && native.flagPresent.count()==4,"unproven native fields marked present");
    for(unsigned i=0;i<80;++i)Check(!native.valuePresent[i],"native temporal matrix fabricated");
    Check(!native.valuePresent[100] && !native.flagPresent[6],"derived FOV or unmeasured jitter flag called native");
    Check(!HasCompleteCameraConstants(native),"partial native constants passed generation gate");
}
static void InvalidFields() {
    auto input=CompleteConstants();input.clipToPrevClip[1].x=sl::INVALID_FLOAT;
    const auto sentinel=SnapshotStreamlineCamera(input);
    Check(sentinel.values[52]==sl::INVALID_FLOAT,"missing matrix silently replaced with identity");
    Check(!HasCompleteCameraConstants(sentinel),"SL invalid float admitted");
    input=CompleteConstants();input.cameraFar=std::numeric_limits<float>::infinity();
    Check(!HasCompleteCameraConstants(SnapshotStreamlineCamera(input)),"nonfinite camera admitted");
    input=CompleteConstants();input.cameraMotionIncluded=sl::eInvalid;
    const auto flags=SnapshotStreamlineCamera(input);
    Check(flags.flags[1]==uint32_t(sl::eInvalid) && !HasCompleteCameraConstants(flags),"unknown Boolean defaulted");
}

// This fake replaces only the COM/fence boundary: no device, queue or GPU work.
class Fence final:public ID3D12Fence {
    std::atomic<ULONG> references{1};bool* destroyed;
public:
    explicit Fence(bool* flag):destroyed(flag) {}
    ~Fence() { *destroyed=true; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void** result) override {if(result)*result=nullptr;return E_NOINTERFACE;}
    ULONG STDMETHODCALLTYPE AddRef() override {return ++references;}
    ULONG STDMETHODCALLTYPE Release() override {const auto left=--references;if(!left)delete this;return left;}
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,UINT*,void*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,UINT,const void*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID,const IUnknown*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID,void**) override {return E_NOTIMPL;}
    UINT64 STDMETHODCALLTYPE GetCompletedValue() override {return 0;}
    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64,HANDLE) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE Signal(UINT64) override {return E_NOTIMPL;}
};
static void CaptureSignal() {
    bool destroyed=false;auto* fence=new Fence(&destroyed);
    const auto failed=CaptureCompletion::AfterSignal(fence,7,E_FAIL,{1234567},10000000);
    Check(!failed.HasScheduledCopySource() && !failed.fence,"failed queue Signal became readiness evidence");
    auto success=CaptureCompletion::AfterSignal(fence,7,S_OK,{1234567},10000000);
    Check(success.HasScheduledCopySource() && success.fenceValue==7,"successful capture signal missing");
    Check(success.qpc.QuadPart==1234567 && success.qpcFrequency==10000000,"integer QPC sample lost");
    success.colorState=D3D12_RESOURCE_STATE_COMMON;
    Check(!success.HasScheduledCopySource(),"COMMON color accepted as the author's COPY_SOURCE contract");
    Check(!CaptureCompletion::AfterSignal(fence,0,S_OK,{1234567},10000000).HasScheduledCopySource(),"zero fence value accepted");
    Check(!CaptureCompletion::AfterSignal(fence,UINT64_MAX,S_OK,{1234567},10000000).HasScheduledCopySource(),"device-loss sentinel accepted");
    fence->Release();
}
static void CaptureLifetime() {
    bool destroyed=false;auto* fence=new Fence(&destroyed);
    {
        auto first=CaptureCompletion::AfterSignal(fence,7,S_OK,{1234567},10000000);
        const auto retained=first;
        first=CaptureCompletion::AfterSignal(fence,8,S_OK,{7654321},10000000);
        fence->Release();first={};
        Check(!destroyed && retained.fenceValue==7 && retained.qpc.QuadPart==1234567,"retained capture tuple mutated or fence released early");
    }
    Check(destroyed,"capture completion leaked its fence");
}
static unsigned conversionCalls{};static int64_t receivedQpc{};
static XrResult XRAPI_CALL ConvertTime(XrInstance,const LARGE_INTEGER* qpc,XrTime* time) {
    ++conversionCalls;receivedQpc=qpc->QuadPart;*time=9000000007;return XR_SUCCESS;
}
static XrResult XRAPI_CALL FailTime(XrInstance,const LARGE_INTEGER*,XrTime* time) {
    ++conversionCalls;*time=123;return XR_ERROR_RUNTIME_FAILURE;
}
static void CaptureTime() {
    bool destroyed=false;auto* fence=new Fence(&destroyed);
    const auto captured=CaptureCompletion::AfterSignal(fence,7,S_OK,{1234567},10000000);
    const auto instance=reinterpret_cast<XrInstance>(uintptr_t{1});
    Check(!captured.ToSubmissionXrTime(instance,nullptr),"QPC guessed into XrTime without extension");
    Check(!captured.ToSubmissionXrTime(XR_NULL_HANDLE,ConvertTime),"time conversion used no instance");
    Check(conversionCalls==0,"unavailable converter was called");
    const auto time=captured.ToSubmissionXrTime(instance,ConvertTime);
    Check(time && *time==9000000007 && receivedQpc==1234567,"runtime QPC mapping was replaced by arithmetic");
    Check(!captured.ToSubmissionXrTime(instance,FailTime),"failed runtime conversion exposed a stale time");
    fence->Release();
}
int main(int argc,char** argv) {
    try {
        const std::string_view test=argc>1?argv[1]:"";
        if(test=="streamline_fields")StreamlineFields();else if(test=="versioned_fields")VersionedFields();
        else if(test=="native_missing")NativeMissing();else if(test=="invalid_fields")InvalidFields();
        else if(test=="capture_signal")CaptureSignal();else if(test=="capture_lifetime")CaptureLifetime();
        else if(test=="capture_time")CaptureTime();else throw std::runtime_error("unknown test");
        std::cout << "PASS " << test << '\n';return 0;
    } catch(const std::exception& e) {std::cerr << "FAIL " << e.what() << '\n';return 1;}
}

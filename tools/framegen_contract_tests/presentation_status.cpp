#include "Framegen/PresentationStatus.hpp"
#include "Framegen/HardwareTelemetry.hpp"
#include <cstdio>
#include <cstdlib>
using namespace cvr::framegen;
void Check(bool value, const char* reason) {
    if (!value) {std::fprintf(stderr,"FAIL: %s\n",reason);std::exit(1);}
}
int main() {
    NativePolicyStatusTracker policy;
    Check(!policy.Read().known,"unobserved policy must be UNKNOWN, not OFF");
    policy.Observe(0,false,true,3);
    Check(!policy.Read().known,"failed NVIDIA request cannot advertise X4");
    for (unsigned count=1;count<=3;++count) {
        policy.Observe(0,true,true,count);
        auto state=policy.Read();
        Check(state.known && state.enabled && state.multiplier==count+1,"accepted X2/X3/X4 mapping");
        policy.Observe(1,true,false,1);
        Check(policy.Read().enabled && policy.Read().multiplier==count+1,"companion OFF cannot overwrite host policy");
    }
    policy.Observe(0,false,false,1);
    Check(!policy.Read().known,"failed OFF request cannot advertise OFF");
    policy.Observe(0,true,false,3);
    Check(policy.Read().known && !policy.Read().enabled && policy.Read().multiplier==0,"OFF has no active multiplier");
    policy.Observe(0,true,true,0);
    Check(!policy.Read().known,"enabled zero-frame policy is not a confirmed multiplier");
    policy.Observe(0,true,true,0xffffffffu);
    Check(!policy.Read().known,"invalid frame count must not wrap to X0");
    PresentedCameraTracker camera;
    Check(camera.Read(100)==PresentedCamera::Unknown,"camera starts UNKNOWN");
    camera.Accepted(PresentedCamera::Paired,100);
    Check(camera.Read(200)==PresentedCamera::Paired,"accepted paired mode");
    Check(camera.Read(1101)==PresentedCamera::Unknown,"stopped submissions must not retain active camera label");
    camera.Accepted(PresentedCamera::Alternating,1200);
    Check(camera.Read(1250)==PresentedCamera::Alternating,"accepted AER transition");
    camera.Accepted(PresentedCamera::Unknown,1300);
    Check(camera.Read(1301)==PresentedCamera::Unknown,"menu/invalid scope clears confirmed stereo mode");
    PresentedGenerationTracker generation;
    generation.Camera(PresentedCamera::Paired);
    Check(!generation.RecentDelivery(PresentedCamera::Paired,50),"own/shared FPS cannot prove a native delivery");
    generation.Generated(PresentedCamera::Paired,0,60);
    Check(generation.RecentDelivery(PresentedCamera::Paired,70),"unknown SDK policy must not hide a confirmed native delivery");
    generation.Camera(PresentedCamera::Alternating);
    Check(!generation.RecentDelivery(PresentedCamera::Alternating,80),"old-camera G cannot activate a new camera mode");
    generation.Generated(PresentedCamera::Paired,0,85);
    Check(!generation.RecentDelivery(PresentedCamera::Alternating,90),"late wrong-camera G was accepted");
    generation.Generated(PresentedCamera::Alternating,0,95);
    Check(!generation.RecentDelivery(PresentedCamera::Alternating,2096),"stopped native delivery remained active");
    Check(!generation.RecentDelivery(PresentedCamera::Alternating,94),"regressing display time remained active");
    generation={};
    generation.Camera(PresentedCamera::Alternating);
    policy.Observe(0,true,true,1);auto old=policy.Read();
    generation.Generated(PresentedCamera::Alternating,old.revision,100);
    const auto queuedRevision=ProducedPolicyRevision(old,old,1);
    Check(queuedRevision==old.revision,"producer receipt retains original options revision");
    NativePolicyStatusTracker roundTrip;roundTrip.Observe(0,true,true,1);const auto beforeRoundTrip=roundTrip.Read();
    roundTrip.Observe(0,true,false,1);roundTrip.Observe(0,true,true,1);
    Check(ProducedPolicyRevision(beforeRoundTrip,roundTrip.Read(),1)==0,"policy change during production (X2-off-X2) is unconfirmed");
    Check(generation.Active(old,PresentedCamera::Alternating,110),"accepted X2 G is active");
    policy.Observe(0,true,true,1);
    Check(policy.Read().revision==old.revision,"same options must not invalidate G each frame");
    policy.Observe(0,true,true,3);
    Check(!generation.Active(policy.Read(),PresentedCamera::Alternating,120),"X4 must wait for its own G");
    Check(ProducedPolicyRevision(old,policy.Read(),1)==0,"policy change during production is unconfirmed");
    Check(ProducedPolicyRevision(policy.Read(),policy.Read(),1)==0,"old X2 packet count cannot match current X4");
    generation.Generated(PresentedCamera::Alternating,queuedRevision,130);
    Check(!generation.Active(policy.Read(),PresentedCamera::Alternating,140),"late old X2 G cannot activate X4");
    generation.Generated(PresentedCamera::Alternating,policy.Read().revision,150);
    Check(generation.Active(policy.Read(),PresentedCamera::Alternating,160),"first X4 G activates X4");
    generation.Camera(PresentedCamera::Paired);
    Check(!generation.Active(policy.Read(),PresentedCamera::Paired,170),"new camera with only real frames waits");
    generation.Generated(PresentedCamera::Paired,policy.Read().revision,180);
    Check(generation.Active(policy.Read(),PresentedCamera::Paired,190),"accepted G for new camera");
    generation.Camera(PresentedCamera::Unknown);generation.Camera(PresentedCamera::Paired);
    Check(!generation.Active(policy.Read(),PresentedCamera::Paired,200),"returning from menu cannot reuse old active status");
    HardwareStatistics hardware;hardware.sequence=1;hardware.sampledAtMs=100;hardware.gpuUsage=100;
    hardware.gpuMemoryValid=hardware.gpuProcessMemoryValid=true;hardware.ramTotal=16;hardware.appWorkingSet=9;
    ValidateHardwareFreshness(hardware,1100);
    Check(hardware.fresh && hardware.gpuUsage==100,"fresh GPU reading retained");
    ValidateHardwareFreshness(hardware,3101);
    Check(!hardware.fresh && hardware.gpuUsage<0 && !hardware.gpuMemoryValid && hardware.ramTotal==0,"stale hardware cannot appear live");
    std::puts("PASS: native policy X2/X3/X4, errors, companion isolation, accepted camera and stale state");
}

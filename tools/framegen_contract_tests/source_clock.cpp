#include "Framegen/SourceClock.hpp"
#include <iostream>
#include <stdexcept>
using namespace cvr::framegen;
static void Check(bool value,const char* reason) {if(!value)throw std::runtime_error(reason);}
int main() {
    try {
        SourceClock clock;const auto first=clock.Generation();
        Check(!clock.AtCameraApplication(1000,7),"unsupported/uninitialized mapping gained a clock");
        cpvr::XrClockMapping runtime{1000,9'000'000'000,1000,true};
        Check(clock.Publish(runtime,7,first),"runtime mapping not retained");
        auto event=clock.AtCameraApplication(1011,7);
        Check(event && event->qpc==1011 && event->xrTime==9'011'000'000 && event->spaceEpoch==7,
            "actual camera QPC was not mapped through the runtime anchor");
        Check(!clock.AtCameraApplication(1011,8),"old origin reused after recenter");
        Check(!clock.AtCameraApplication(3001,7),"stale clock mapping accepted");
        clock.Invalidate();Check(!clock.AtCameraApplication(1011,7),"instance/session reset kept old mapping valid");
        Check(!clock.Publish(runtime,7,first),"in-flight old-instance publisher revived invalidated clock");
        Check(clock.Publish(runtime,7,clock.Generation()),"fresh post-reset mapping rejected");
        event=clock.AtCameraApplication(1011,7);Check(event && event->clockGeneration!=first,"reset epoch lost");
        runtime.valid=false;Check(!clock.Publish(runtime,7,clock.Generation()),"invalid fallback anchor accepted");
        std::cout<<"PASS source_clock\n";return 0;
    } catch(const std::exception& e) {std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}

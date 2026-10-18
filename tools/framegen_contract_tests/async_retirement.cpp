#include "Framegen/AsyncRetirement.hpp"
#include <future>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
using namespace cvr::framegen;
using namespace std::chrono_literals;
void Check(bool pass,const char* why){if(!pass){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}}
int main(){
    AsyncRetirement retire;Check(retire.Poll()==RetirementState::Idle,"cold owner starts work");
    std::promise<void> release;auto gate=release.get_future().share();
    auto caller=std::async(std::launch::async,[&]{return retire.Begin([gate]{gate.wait();return true;});});
    const bool returned=caller.wait_for(100ms)==std::future_status::ready;
    if(!returned){release.set_value();caller.get();Check(false,"XR caller waits for backend stop");}
    Check(caller.get(),"stop was not started");
    Check(retire.Poll()==RetirementState::Pending,"incomplete stop advertised complete");
    Check(!retire.Reset()&&!retire.Begin([]{return true;}),"new owner overlaps unfinished stop");
    release.set_value();Check(retire.Finish()==RetirementState::Complete,"finished stop not acknowledged");
    Check(retire.Reset()&&retire.Poll()==RetirementState::Idle,"clean owner cannot restart");
    Check(retire.Begin([]{return false;}),"failed cleanup probe not scheduled");
    Check(retire.Finish()==RetirementState::Failed&&!retire.Reset()&&!retire.Begin([]{return true;}),"unsafe cleanup permits a new owner");
    AsyncRetirement exception;Check(exception.Begin([]()->bool{throw std::runtime_error("cleanup");}),"exception probe not scheduled");
    Check(exception.Finish()==RetirementState::Failed,"cleanup exception escaped or was called success");
    std::puts("PASS async stop, nonblocking caller, pending exclusion, clean restart, failed cleanup retention");
}

#define CVR_CPU_STAGE_TEST
#include "Framegen/CpuStageTiming.hpp"
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
void Log(const char*,...){}
using namespace cvr::framegen::cpu_timing;
void Check(bool ok,const char* why){if(!ok){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}}
int main(){
    unsigned originalCalls{};int result{};
    testClock=100;
    {Scope outer(Stage::Ngx11Total);testClock=110;
        {Scope child(Stage::NgxHost);result=[&]{++originalCalls;testClock=130;return 123;}();}
        testClock=150;outer.Finish();testClock=160;}
    auto s=Take();Check(originalCalls==1&&result==123,"instrumentation preserves original count/result");
    Check(s[unsigned(Stage::Ngx11Total)].calls==1&&s[unsigned(Stage::Ngx11Total)].ticks==50,"explicit finish/destructor counted once");
    Check(s[unsigned(Stage::NgxHost)].ticks==20,"nested inclusive stages remain separate");
    testEnabled=false;{Scope disabled(Stage::NgxHost);testClock+=500;}s=Take();Check(s[unsigned(Stage::NgxHost)].calls==0,"disabled diagnostic does not publish samples");testEnabled=true;
    {Scope skipped(Stage::NgxHost,false);testClock+=500;}s=Take();Check(s[unsigned(Stage::NgxHost)].calls==0,"other NGX features not mislabelled feature11");
    std::vector<std::thread> writers;for(unsigned i=0;i<4;++i)writers.emplace_back([]{for(unsigned j=0;j<1000;++j)Record(Stage::NgxHost,j);});for(auto& t:writers)t.join();
    s=Take();Check(s[unsigned(Stage::NgxHost)].calls==4000&&s[unsigned(Stage::NgxHost)].ticks==1998000&&s[unsigned(Stage::NgxHost)].peak==999,"concurrent recording preserves sample count sum peak");
    Check(Take()[unsigned(Stage::NgxHost)].calls==0,"snapshot clears exactly once");
    std::puts("PASS: CPU wall timing scope, disabled path, original call/result, concurrent accumulator");
}

#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#ifndef CPVR_TEST_OLD_RETIREMENT_ORDER
#include "Framegen/CompletedInputWork.hpp"
#endif

namespace {
struct Input { unsigned id{}; };
struct Slot { std::shared_ptr<Input> input; uint64_t producerFence{}; };
struct Work { uint64_t fence{}; std::shared_ptr<Input> inputs[2]; };
using Works=std::array<Work,3>;
using Pools=std::array<std::array<Slot,4>,2>;
unsigned checks{};
void Check(bool value,const char* reason) {
    if(!value)throw std::runtime_error(reason);
    ++checks;std::printf("PASS: %s\n",reason);
}
void Seed(Pools& pools,Works& work) {
    for(unsigned eye=0;eye<2;++eye)for(unsigned i=0;i<4;++i)
        pools[eye][i]={std::make_shared<Input>(Input{eye*4+i}),i<3?1u:12u};
    for(unsigned i=0;i<3;++i) {
        work[i].fence=8+i;
        for(unsigned eye=0;eye<2;++eye)work[i].inputs[eye]=pools[eye][i].input;
    }
}
bool Reusable(const Slot& slot,uint64_t completed) {
    return completed!=std::numeric_limits<uint64_t>::max()&&slot.input.use_count()==1&&completed>=slot.producerFence;
}
bool InputsAvailable(const Pools& pools,uint64_t completed) {
    for(const auto& pool:pools) {
        bool available=false;
        for(const auto& slot:pool)available=available||Reusable(slot,completed);
        if(!available)return false;
    }
    return true;
}
void OldGenerateCollection(Works& work,uint64_t completed) {
    // Exact old Generator.cpp cleanup predicate, reached only inside Generate.
    for(auto& old:work)if(old.fence&&completed>=old.fence)
        {old.inputs[0].reset();old.inputs[1].reset();}
}
bool TickBeforeSelectingInputs(Pools& pools,Works& work,uint64_t completed) {
#ifdef CPVR_TEST_OLD_RETIREMENT_ORDER
    // Existing order: missing current inputs returns before Generate can retire.
    if(!InputsAvailable(pools,completed))return false;
    OldGenerateCollection(work,completed);
#else
    cvr::framegen::RetireCompletedInputWork(work,completed);
#endif
    return InputsAvailable(pools,completed);
}
}

int main() {
    try {
        Pools pools{};Works work{};Seed(pools,work);
        Check(!InputsAvailable(pools,11),"Three completed Work leases plus fourth pending producer block both pools");
        Check(TickBeforeSelectingInputs(pools,work,11),"Tick retires completed Work leases even without current inputs; future guides are admitted");
        Check(!Reusable(pools[0][3],11)&&!Reusable(pools[1][3],11),"Pending producer slots remain unavailable");
        for(const auto& item:work)Check(!item.inputs[0]&&!item.inputs[1],"Completed Work releases both input aliases");
#ifndef CPVR_TEST_OLD_RETIREMENT_ORDER
        Check(cvr::framegen::RetireCompletedInputWork(work,11)==0,"Repeated retirement is idempotent");

        Seed(pools,work);work[1].fence=12;work[2].fence=5;
        Check(cvr::framegen::RetireCompletedInputWork(work,8)==4,"Only completed Work leases retire, including out-of-order completions");
        Check(work[1].inputs[0]&&work[1].inputs[1]&&work[1].fence==12,"Incomplete Work and its fence identity remain held");
        Check(cvr::framegen::RetireCompletedInputWork(work,12)==2,"Later fence progress retires the formerly incomplete Work");

        Seed(pools,work);work[0].fence=0;
        Check(cvr::framegen::RetireCompletedInputWork(work,10)==4&&work[0].inputs[0]&&work[0].inputs[1],"Unsignalled Work is retained");
        Check(cvr::framegen::RetireCompletedInputWork(work,std::numeric_limits<uint64_t>::max())==0&&work[0].inputs[0],"Device-removed completion sentinel cannot release inputs");

        Seed(pools,work);auto external=pools[0][0].input;
        Check(cvr::framegen::RetireCompletedInputWork(work,11)==6,"Work collection releases only its own six aliases");
        Check(!Reusable(pools[0][0],11)&&Reusable(pools[0][1],11),"An independent external owner still blocks its own producer slot");
        external.reset();Check(Reusable(pools[0][0],11),"Slot becomes reusable only after its final external owner releases");
        Check(work[0].fence==8&&work[1].fence==9&&work[2].fence==10,"Retirement preserves Work fence values and cadence-independent state");
#endif
        std::printf("PASS TOTAL %u\n",checks);return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}
}

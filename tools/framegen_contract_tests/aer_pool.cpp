#include <Windows.h>
#include "Runtimes/CapturePoolPolicy.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
using namespace cvr::capture;
static unsigned checks;
static void check(bool ok,const char* s){++checks;if(!ok){std::fprintf(stderr,"FAIL %s\n",s);std::exit(1);}}
int main(){
    std::array<uintptr_t,8> pool{1,2,3,4,5,6,7,8};
    BufferLeases leases;leases.Pin(1);leases.Pin(2);
    check(!ReserveMainWrite(std::span(pool).first(3),3,2,leases),"old three-cell pool deadlocks with two accepted and one ineligible published source");
    check(MainPoolCapacity(false,false)==3 && MainPoolCapacity(true,false)==8 && MainPoolCapacity(true,true)==8,"bounded capacity covers asynchronous AER capture and display owners");
    auto s=ReserveMainWrite(std::span(pool).first(4),3,2,leases);
    check(s && pool[*s]==4,"AER has a spare to publish missing opposite eye");leases.EndWrite(4);
    for(uintptr_t i=3;i<=6;++i)leases.Pin(i);
    check(!ReserveMainWrite(std::span(pool).first(7),7,6,leases),"six independent persistent pins plus publication need another cell");
    s=ReserveMainWrite(pool,7,6,leases);check(s && pool[*s]==8,"AER plus NVIDIA can progress without forced unpin");
    check(!ReserveMainWrite(pool,7,6,leases),"in-flight writer cannot be selected twice");leases.EndWrite(8);
    leases.Pin(8);check(!ReserveMainWrite(pool,7,6,leases),"GPU busy may skip but cannot overwrite");leases.Unpin(8);
    check(bool(ReserveMainWrite(pool,7,6,leases)),"completed transient GPU consumer frees writer");leases.EndWrite(8);
    for(uintptr_t i=1;i<=6;++i)leases.Unpin(i);
    for(unsigned i=0;i<1000;++i) {
        const auto published=pool[i%8];s=ReserveMainWrite(pool,published,i%8,leases);
        check(s && pool[*s]!=published,"steady writer preserves current publication");leases.EndWrite(pool[*s]);
    }
    std::printf("PASS %u actual capture lease/pool checks\n",checks);
}

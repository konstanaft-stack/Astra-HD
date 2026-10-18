#include "Framegen/GuideCapturePolicy.hpp"
#include <cstdio>
#include <cstdlib>
static void Check(bool ok,const char* why){if(!ok){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}}
int main(){using cvr::framegen::NeedGuideCopies;
    Check(!NeedGuideCopies(true,false,false,false,false,false),"plain stereo projection must not force depth/MV copies");
    Check(!NeedGuideCopies(true,false,false,true,true,false),"known disabled game FG needs projection only");
    Check(NeedGuideCopies(true,true,false,false,false,false),"AER SR history keeps required guides with FG off");
    Check(NeedGuideCopies(true,false,true,false,false,false),"owned generation requires guides");
    Check(NeedGuideCopies(true,false,false,true,false,false),"unknown native FG conservatively retains guides");
    Check(NeedGuideCopies(true,false,false,true,true,true),"active native FG requires guides");
    Check(!NeedGuideCopies(false,true,true,true,false,true),"stopped projection producer does not capture");
    std::puts("PASS: stereo off, AER history, native unknown/on/off, owned FG, stopped capture");
}

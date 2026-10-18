#include "MenuPreview/UiAliasingPolicy.hpp"
#include <cstdio>
#include <limits>
using namespace cvr::menu_preview;
int main(){
 int failures=0;
 auto check=[&](bool value,const char* label){if(!value){std::printf("FAIL %s\n",label);++failures;}};
 const UiAllocation portrait{2,71,0x40000,0x10000};
 const UiAllocation otherHeap{2,72,0x40000,0x10000},adjacent{2,71,0x50000,0x10000};
 const UiAllocation overlap{2,71,0x48000,0x10000},unknown{},committed{1};
 check(!UiAliasInvalidates(1,portrait,0,unknown,2,otherHeap),"unrelated heap activation must preserve portrait");
 check(!UiAliasInvalidates(1,portrait,0,unknown,2,adjacent),"adjacent non-overlapping allocation must preserve portrait");
 check(UiAliasInvalidates(1,portrait,0,unknown,2,overlap),"overlapping activation must invalidate portrait");
 check(UiAliasInvalidates(1,portrait,0,unknown,2,unknown),"unknown destination must remain conservative");
 check(UiAliasInvalidates(1,portrait,0,unknown,0,unknown),"global alias must remain conservative");
 check(UiAliasInvalidates(1,portrait,1,portrait,2,otherHeap),"explicit source must invalidate");
 check(UiAliasInvalidates(1,portrait,2,otherHeap,1,portrait),"explicit destination must invalidate");
 check(!UiAliasInvalidates(1,committed,0,unknown,2,unknown),"committed resource has isolated allocation");
 check(!UiAliasInvalidates(1,portrait,2,otherHeap,0,unknown),"disjoint explicit source with wildcard destination");
 check(UiAliasInvalidates(1,unknown,0,unknown,2,otherHeap),"unknown portrait allocation");
 const UiAllocation overflow{2,71,std::numeric_limits<uint64_t>::max()-2,16};
 check(UiAliasInvalidates(1,portrait,0,unknown,2,overflow),"overflowing range is not evidence of disjointness");
 std::printf("%s %d failures\n",failures?"FAIL":"PASS",failures);return failures?1:0;
}

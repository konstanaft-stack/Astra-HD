#include "Runtimes/CaptureContextFences.hpp"
#include "Runtimes/CaptureBufferLeases.hpp"
#include <cassert>
#include <iostream>
using namespace cvr::capture;
int main(){CaptureContextFences fences;assert(fences.CanReset(0,0));
 fences.Executed(0,4,true);fences.Executed(1,5,true);fences.Executed(2,6,true);
 assert(fences.CanReset(0,4)); // menu rotates to0, records, but fails validation before Execute: no fence update
 assert(fences.Required(0)==4);assert(!fences.CanReset(1,4)); // fallback rotates to1; global6-2=4 would incorrectly permit reset
 assert(fences.CanReset(1,5));fences.Executed(1,7,true);assert(!fences.CanReset(2,5));assert(fences.CanReset(2,6));
 assert(fences.AllRequired()==7);assert(!fences.CanReset(1,6));assert(fences.CanReset(1,7));
 fences.Executed(2,8,false);assert(!fences.CanReset(2,100)&&fences.AllRequired()==UINT64_MAX);assert(!fences.CanReset(0,UINT64_MAX));
 fences.Cleared();assert(fences.CanReset(2,0));
 BufferLeases leases;assert(leases.BeginWrite(100));leases.EndWrite(100);assert(leases.Pin(100)); // resource publish ownership independent of allocator fence
 assert(!leases.BeginWrite(100));leases.Unpin(100);assert(leases.BeginWrite(100));
 std::cout<<"PASS multi-path failed recording/actual slot fence; no early Reset; failed Signal/device removal quarantine; resize all-slot fence\n";
}

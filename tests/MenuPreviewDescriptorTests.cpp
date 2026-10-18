#include "MenuPreview/DescriptorProvenancePolicy.hpp"
#include <cassert>
#include <iostream>
using namespace cvr::menu_preview;
int main(){
 PreviewDescriptorLedger ledger;assert(!ledger.Read(1000));auto heap=ledger.RegisterHeap(10,1,1000,4,32);assert(heap);
 auto resource=ledger.RegisterResource(100);auto token=ledger.BeginMutation(1);ledger.AddDestination(token,1000,1,32);assert(!ledger.Read(1000));assert(ledger.FinishDirect(token,1000,100,resource));
 auto before=ledger.Read(1000);assert(before&&ledger.Validate(*before));assert(MatchPreviewExpectedDescriptor(true,1000,1000));assert(!MatchPreviewExpectedDescriptor(true,1032,1000));assert(!MatchPreviewExpectedDescriptor(false,1000,1000));
 token=ledger.BeginMutation(1);ledger.AddDestination(token,1000,1,32);assert(!ledger.Read(1000)&&!ledger.Validate(*before));assert(ledger.FinishDirect(token,1000,100,resource));assert(*ledger.Read(1000)!=*before); // OM before/after version change rejects
 auto current=*ledger.Read(1000);token=ledger.BeginMutation(1);ledger.AddDestination(token,1032,1,32);assert(ledger.Validate(current));ledger.FinishCopies(token);assert(ledger.Validate(current)); // unrelated mutation doesn't poison bound slot
 token=ledger.BeginMutation(1);ledger.AddDestination(token,1000,2,32);ledger.FinishCopies(token);assert(!ledger.Read(1000)); // copies remain unknown
 token=ledger.BeginMutation(1);ledger.AddDestination(token,1000,1,32);assert(ledger.FinishDirect(token,1000,100,resource));
 auto other=ledger.BeginMutation(1);ledger.AddDestination(other,1000,1,32);auto overlap=ledger.BeginMutation(1);ledger.AddDestination(overlap,1000,1,32);
 assert(!ledger.FinishDirect(other,1000,100,resource));assert(!ledger.FinishDirect(overlap,1000,100,resource));assert(!ledger.Read(1000));
 token=ledger.BeginMutation(1);ledger.AddDestination(token,1000,1,32);assert(ledger.FinishDirect(token,1000,100,resource));
 current=*ledger.Read(1000);ledger.ForgetResource(100,resource);assert(!ledger.Read(1000));const auto resource2=ledger.RegisterResource(100);assert(resource2!=resource);assert(!ledger.Validate(current));
 token=ledger.BeginMutation(1);ledger.AddDestination(token,1000,1,32);assert(ledger.FinishDirect(token,1000,100,resource2));
 current=*ledger.Read(1000);token=ledger.BeginMutation(1);ledger.AddDestination(token,1000,1,32);ledger.ForgetHeap(10,heap);heap=ledger.RegisterHeap(10,1,1000,4,32);assert(!ledger.FinishDirect(token,1000,100,resource2));assert(!ledger.Validate(current)); // heap reuse during unlocked driver call
 token=ledger.BeginMutation(1);ledger.AddDestination(token,9999,1,32);assert(!ledger.FinishDirect(token,9999,100,resource2));assert(!ledger.Read(9999));
 token=ledger.BeginMutation(2);ledger.AddDestination(token,1000,1,32);assert(!ledger.FinishDirect(token,1000,100,resource2)); // foreign device
 PreviewDescriptorLedger bounded;for(uintptr_t n=1;n<=128;++n)assert(bounded.RegisterHeap(n,1,n*4096,1,32));assert(!bounded.RegisterHeap(129,1,129*4096,1,32));assert(!bounded.Read(4096));
 std::cout<<"PASS direct slot proof; exact expected handle; pending/version OM snapshot; copies unknown; overlapping writers; heap/resource reuse; foreign device; bounds\n";
}

#include "MenuPreview/DescriptorProvenancePolicy.hpp"
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace cvr::menu_preview;
void Require(bool value,const char* message){if(!value){std::fprintf(stderr,"FAIL %s\n",message);std::exit(1);}}
int main(){
    PreviewDescriptorLedger ledger;
    Require(ledger.RegisterHeap(10,1,0x1000,2048,16)!=0,"first heap");
    Require(ledger.RegisterHeap(20,2,0x20000,2048,16)!=0,"second device heap");
    const auto generation=ledger.RegisterResource(100);
    auto write=[&](uintptr_t device,uintptr_t handle){
        const auto token=ledger.BeginMutation(device);
        ledger.AddDestination(token,handle,1,16);
        Require(ledger.FinishDirect(token,handle,100,generation),"direct RTV write");
    };
    std::vector<PreviewDescriptorProof> first,second;
    for(unsigned i=0;i<512;++i){
        const auto a=uintptr_t(0x1000)+i*48,b=uintptr_t(0x20000)+i*48;
        write(1,a);write(2,b);first.push_back(*ledger.Read(a));second.push_back(*ledger.Read(b));
    }
    const auto single=ledger.BeginMutation(1);
    ledger.AddDestination(single,first[19].handle,1,16);
    Require(!ledger.Read(first[19].handle),"single target invalidated before completion");
    for(unsigned i=0;i<first.size();++i)if(i!=19)Require(ledger.Validate(first[i]),"single copy damaged unrelated RTV");
    ledger.FinishCopies(single);
    Require(!ledger.Read(first[19].handle),"copy completion revived old target");

    const auto start=first[30].handle;
    const auto sparse=ledger.BeginMutation(1);
    ledger.AddDestination(sparse,start,9,32);
    for(unsigned i=0;i<first.size();++i){
        const auto h=first[i].handle;
        const bool affected=h>=start&&(h-start)%32==0&&(h-start)/32<9;
        Require(bool(ledger.Read(h))==(i!=19&&!affected),"sparse stride invalidation mismatch");
    }
    ledger.FinishCopies(sparse);

    const auto handle=first[70].handle;
    const auto copy=ledger.BeginMutation(1);ledger.AddDestination(copy,handle,1,16);
    const auto direct=ledger.BeginMutation(1);ledger.AddDestination(direct,handle,1,16);
    Require(!ledger.FinishDirect(direct,handle,100,generation),"overlapping late direct was admitted");
    ledger.FinishCopies(copy);Require(!ledger.Read(handle),"overlap completion revived old target");
    write(1,handle);Require(!ledger.Validate(first[70]),"new direct write reused old proof");

    const auto broad=ledger.BeginMutation(1);
    ledger.AddDestination(broad,0x1000,20000,16);
    for(const auto& proof:first)Require(!ledger.Read(proof.handle),"large range retained affected RTV");
    for(const auto& proof:second)Require(ledger.Validate(proof),"range invalidated another device");
    ledger.FinishCopies(broad);
    const auto old=second[100];ledger.ForgetHeap(20,old.heapGeneration);
    Require(ledger.RegisterHeap(20,2,0x20000,2048,16)!=0,"heap reuse");
    write(2,old.handle);Require(!ledger.Validate(old),"heap reuse admitted old generation");
    std::puts("PASS single/sparse/large RTV ranges, other-device isolation, pending overlap, late completion and heap reuse");
}

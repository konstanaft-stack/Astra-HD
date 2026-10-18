#include "MenuPreview/DescriptorMap.hpp"
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
using namespace cvr::menu_preview;
void Require(bool value,const char* message){if(!value){std::fprintf(stderr,"FAIL %s\n",message);std::exit(1);}}
int main(){
    DescriptorMap<int,6> map;
    std::unordered_map<uintptr_t,int> reference;
    uint32_t random=17;
    for(unsigned n=0;n<50000;++n){
        random=random*1664525u+1013904223u;const auto key=uintptr_t(random%2048);
        if(random&0x80000000u){map[key]=int(n);reference[key]=int(n);}
        else Require(map.erase(key)==reference.erase(key),"erase result");
        Require(map.size()==reference.size(),"size");
        for(const auto probe:{key,uintptr_t(0),uintptr_t(2049)}){
            const auto actual=map.find(probe),expected=reference.find(probe);
            Require((actual==map.end())==(expected==reference.end()),"false negative or invented entry");
            if(expected!=reference.end())Require(actual->second==expected->second&&map.MayContain(probe),"value or membership");
        }
    }
    map[90000]=71;auto* stable=&map.find(90000)->second;
    map.reserve(10000);for(uintptr_t n=100000;n<110000;++n)map.emplace(n,12);
    Require(stable==&map.find(90000)->second&&*stable==71,"rehash invalidated existing value reference");
    auto node=map.extract(90000);Require(!node.empty()&&map.find(90000)==map.end(),"extract");
    node.key()=90001;node.mapped()=72;
    Require(map.insert(std::move(node)).inserted&&map.find(90001)->second==72,"rekey insertion");
    const auto duplicate=map.emplace(90001,99);Require(!duplicate.second&&duplicate.first->second==72,"duplicate insert");
    map.clear();Require(map.size()==0&&!map.MayContain(90001),"clear");
    // Tiny filter intentionally saturates. Saturation must stay conservative
    // through erasure, rather than wrap to zero and hide a surviving key.
    DescriptorMap<int,1> saturated;
    for(uintptr_t n=0;n<150000;++n)saturated.emplace(n,13);
    for(uintptr_t n=0;n<149999;++n)saturated.erase(n);
    Require(saturated.find(149999)!=saturated.end()&&saturated.find(149999)->second==13,"saturation lost survivor");
    auto retained=saturated.extract(149999);retained.key()=999999;saturated.insert(std::move(retained));
    Require(saturated.find(999999)!=saturated.end(),"saturated rekey");
    saturated.clear();Require(!saturated.MayContain(999999),"saturated clear");
    std::puts("PASS descriptor map mutation, erase, stable references, node rekey, duplicates, collision saturation and clear");
}

#include <Windows.h>
#include <xmmintrin.h>
#include "Hooks/NativeSettingsPreflight.hpp"
#include "native-clone-ranges.hpp"
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <vector>
using namespace cvr::hooks;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Clone {
    uint8_t* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x1D57000,MEM_RESERVE,PAGE_NOACCESS));
    ~Clone(){if(base)VirtualFree(base,0,MEM_RELEASE);}
};
struct FloatingEnvironment {
    unsigned previous=_mm_getcsr();
    ~FloatingEnvironment(){_mm_setcsr(previous);}
};
int wmain(int argc,wchar_t**argv) try {
    Check(argc==2,"native clone directory missing");
    Clone clone;Check(clone.base!=nullptr,"reserve sparse native image");
    std::set<uint32_t> pages;
    for(const auto& range:cloneRanges)for(uint32_t page=range.rva&~0xfffu;page<range.rva+range.size;page+=4096)pages.insert(page);
    for(auto page:pages){Check(VirtualAlloc(clone.base+page,4096,MEM_COMMIT,PAGE_READWRITE)!=nullptr,"commit native code page");std::memset(clone.base+page,0xcc,4096);}
    for(const auto&range:cloneRanges){
        std::ifstream source(std::filesystem::path(argv[1])/range.file,std::ios::binary);
        source.read(reinterpret_cast<char*>(clone.base+range.rva),range.size);
        Check(source.gcount()==range.size,"short native block");
    }
    for(auto page:pages){DWORD old{};Check(VirtualProtect(clone.base+page,4096,PAGE_EXECUTE_READ,&old)!=0,"protect native code");}
    Check(FlushInstructionCache(GetCurrentProcess(),clone.base,0x1D57000)!=0,"flush native code");
    // Only the reviewed public predicate entries are callable. In particular,
    // the unrelated DXGI function21C3F4 inside a copied slab is never invoked.
    constexpr std::array<uint32_t,12> entries{0x21C584,0x21C520,0x21C53C,0x21C508,0x21C4C0,0x21C478,
        0x1D56D90,0x1D56DE0,0x21C3E4,0x21C438,0x1D56E70,0x1D56E30};
    using Predicate=bool(*)(const void*);
    FloatingEnvironment fp;
    unsigned cases=0,comparisons=0;
    auto verify=[&](NativeSettingsSnapshot snapshot,unsigned csr=0x1f80u){
        const auto before=snapshot.bytes;
        std::array<bool,12> native{};
        for(size_t i=0;i<entries.size();++i){
            _mm_setcsr(csr);
            native[i]=reinterpret_cast<Predicate>(clone.base+entries[i])(snapshot.bytes.data());
            Check(snapshot.bytes==before,"native predicate modified settings");
        }
        _mm_setcsr(csr);
        const auto actual=InspectNativeSettings(snapshot);
        for(size_t i=0;i<native.size();++i){
            if(actual.changed[i]!=native[i]){
                std::cerr<<"predicate mismatch rva="<<std::hex<<entries[i]<<" case="<<std::dec<<cases<<'\n';
                throw std::runtime_error("production preflight differs from native helper");
            }
            ++comparisons;
        }
        const bool valid=uint32_t(snapshot.U32(0x38)-1u)<=0x7ffffffeu;
        bool common=false;for(size_t i=2;i<=10;++i)common|=native[i];
        const bool expected=valid&&(snapshot.U8(0x20)==3
            ? native[0]||snapshot.U8(0x10f)!=0||native[1]||common
            : snapshot.U8(0x10f)!=0||snapshot.U8(0x92)!=snapshot.U8(0x3c)||native[0]||
              snapshot.U8(0x90)!=snapshot.U8(0x20)||snapshot.U32(0x8c)!=snapshot.U32(0x78)||native[1]||common||native[11]);
        Check(actual.validDevice==valid&&actual.willResize==expected,"aggregate decision differs from reviewed native branches");
        Check(snapshot.bytes==before,"production preflight modified settings");++cases;
    };
    NativeSettingsSnapshot stable;stable.PutU32(0x38,1);
    verify(stable);
    for(size_t byte=0;byte<stable.bytes.size();++byte)for(uint8_t value:{uint8_t(1),uint8_t(0x80),uint8_t(0xff)}){
        auto sample=stable;sample.bytes[byte]=value;verify(sample);
        sample.bytes[0x20]=sample.bytes[0x90]=3;verify(sample);
    }
    constexpr std::array<uint32_t,12> floats{0,0x80000000,0x3f800000,0xbf800000,1,0x007fffff,
        0x7f800000,0xff800000,0x7fc00000,0x7f800001,0xffc00000,0x00800000};
    constexpr std::array<std::array<size_t,2>,6> pairs{{{0xd4,0x158},{0xd8,0x114},{0xdc,0x124},
        {0xe4,0x138},{0xec,0x144},{0xf4,0x150}}};
    for(auto pair:pairs)for(auto left:floats)for(auto right:floats)for(auto csr:{0x1f80u,0x9fc0u}){
        auto sample=stable;sample.PutU32(pair[0],left);sample.PutU32(pair[1],right);verify(sample,csr);
    }
    for(uint32_t index:{0u,1u,0x7fffffffu,0x80000000u,0xffffffffu})for(uint8_t mode:{uint8_t(0),uint8_t(1),uint8_t(2),uint8_t(3)}){
        auto sample=stable;sample.PutU32(0x38,index);sample.bytes[0x20]=sample.bytes[0x90]=mode;
        sample.bytes[0x10f]=1;verify(sample);
    }
    for(size_t i=0;i<pairs.size();++i){
        auto sample=stable;sample.PutU32(pairs[i][0],0x7f800001);
        if(i!=1)sample.PutU32(pairs[i][0]-4,1); // integer mismatch still wins over unordered float
        verify(sample);
    }
    Check(!RequiresNativeSettingsAdmission(stable,{}),"unchanged pass requires GPU reservation");
    NativeDimensionWrite author{NativeDimensionWrite::Kind::Author,{1600,1728}};
    Check(RequiresNativeSettingsAdmission(stable,author),"author mutation hidden by unchanged current predicate");
    NativeDimensionWrite pending{NativeDimensionWrite::Kind::Pending,{1600,1728}};
    Check(RequiresNativeSettingsAdmission(stable,pending),"pending PPD mutation missed");
    stable.PutU32(0x18,1600);stable.PutU32(0x1c,1728);stable.PutU32(0x84,1600);stable.PutU32(0x88,1728);
    Check(!RequiresNativeSettingsAdmission(stable,author)&&!RequiresNativeSettingsAdmission(stable,pending),
          "repeated identical dimension writes cause per-frame reservation");
    std::cout<<"PASS "<<cases<<" snapshots / "<<comparisons<<" native helper comparisons, read-only decision, NaNs/DAZ and planned dimension writes\n";
    return 0;
}catch(const std::exception&error){std::cerr<<error.what()<<'\n';return 1;}

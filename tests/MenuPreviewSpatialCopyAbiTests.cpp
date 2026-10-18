#include "MenuPreview/SpatialEditorSource.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
extern "C" void* SpatialCopyOriginalFixture(void*,const void*,size_t);
extern "C" int RunSpatialCopyAbi(void*,const void*,size_t);
extern "C" {
std::atomic<uint32_t> CvrEditorCopyActive{};
void* CvrEditorCopyOriginal=reinterpret_cast<void*>(&SpatialCopyOriginalFixture);
uint32_t SpatialCopyOriginalSawExpanded{};
}
unsigned calls{};
extern "C" void CvrEditorCopyBefore(void* source,size_t size)noexcept{
    ++calls;cvr::menu_preview::EditorQuad result;
    cvr::menu_preview::EditorQuad before{};
    if(size!=sizeof(before))return;
    std::memcpy(before.data(),source,size);
    if(cvr::menu_preview::ExpandEditorPreview(std::as_bytes(std::span(before)),1968,2192,result))
        cvr::menu_preview::WriteEditorSourceIfUnchanged(source,before,result);
}
void require(bool value,const char* message){if(!value){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
int main(){
    using namespace cvr::menu_preview;EditorQuad input{},output{};
    const float x[]{0,1517,1517,0,1517,0},y[]{542.5f,542.5f,1649.5f,542.5f,1649.5f,1649.5f};
    for(unsigned i=0;i<6;++i){input[i].x=x[i];input[i].y=y[i];input[i].u=x[i]?0x3c00:0;input[i].v=y[i]>600?0x3c00:0;}
    require(RunSpatialCopyAbi(output.data(),input.data(),sizeof(input))==1,"inactive memcpy preserves original return, volatile outputs and flags");
    require(calls==0&&!std::memcmp(input.data(),output.data(),sizeof(input)),"inactive world copy skips editor callback and preserves bytes");
    const auto original=input;CvrEditorCopyActive=1;
    require(RunSpatialCopyAbi(output.data(),input.data(),sizeof(input))==1,"editor callback preserves original return, volatile outputs and flags");
    require(SpatialCopyOriginalSawExpanded==1,"native copy must receive the already expanded source, matching Luke relay ordering");
    require(calls==1&&input[0].y==0&&input[2].x==1968&&input[2].y==2192&&!std::memcmp(input.data(),output.data(),sizeof(input)),"expanded source is copied by original engine code");
    require(RunSpatialCopyAbi(output.data(),input.data(),64)==1&&calls==1,"other memcpy sizes bypass editor callback");
    require(RunSpatialCopyAbi(output.data(),input.data(),sizeof(input))==1&&calls==2&&!std::memcmp(input.data(),output.data(),sizeof(input)),"reused expanded staging quad is copied without applying the transform again");
    auto changed=original;changed[0].color[0]=123;
    require(!WriteEditorSourceIfUnchanged(changed.data(),original,input)&&changed[0].color[0]==123,"source changed since recognition is not overwritten");
    auto* readOnly=VirtualAlloc(nullptr,sizeof(original),MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    require(readOnly!=nullptr,"read-only source fixture allocation");
    std::memcpy(readOnly,original.data(),sizeof(original));DWORD oldProtection{};
    require(VirtualProtect(readOnly,sizeof(original),PAGE_READONLY,&oldProtection)!=0,"source fixture made read-only");
    require(RunSpatialCopyAbi(output.data(),readOnly,sizeof(original))==1&&calls==3&&!std::memcmp(readOnly,original.data(),sizeof(original))&&!std::memcmp(output.data(),original.data(),sizeof(original)),"read-only source remains unchanged and native copy still runs");
    require(VirtualFree(readOnly,0,MEM_RELEASE)!=0,"read-only source fixture cleanup");
    require(!WriteEditorSourceIfUnchanged(reinterpret_cast<void*>(UINTPTR_MAX-32),original,input),"overflowing source range rejected");
    std::puts("PASS native source-before-copy ordering, game memcpy ABI, bypass, reuse and source write guards");
}

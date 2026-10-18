#include "Framegen/InputCopy.hpp"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
using namespace cvr::framegen;
void Check(bool ok,const char* why){if(!ok){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}}
int main(){
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=1920;d.Height=1920;
    d.DepthOrArraySize=1;d.SampleDesc.Count=1;d.MipLevels=1;d.Format=DXGI_FORMAT_D32_FLOAT;
    InputTag tag{};InputExtent out;
    Check(InspectInputDescription(d,tag,out)&&out.width==1920&&out.height==1920&&out.format==DXGI_FORMAT_R32_FLOAT,"full depth metadata");
    tag.x=20;tag.y=30;tag.width=1114;tag.height=1000;
    Check(InspectInputDescription(d,tag,out)&&out.width==1114&&out.height==1000,"actual SR subrectangle, no texture copy needed");
    tag.x=UINT32_MAX;Check(!InspectInputDescription(d,tag,out),"overflow/outside crop rejected");tag.x=20;
    tag.width=1920;Check(!InspectInputDescription(d,tag,out),"crop beyond texture rejected");tag.width=1114;
    d.SampleDesc.Count=4;Check(!InspectInputDescription(d,tag,out),"multisampled guide rejected");d.SampleDesc.Count=1;
    d.DepthOrArraySize=2;Check(!InspectInputDescription(d,tag,out),"array guide rejected");d.DepthOrArraySize=1;
    tag.type=1;
    for(auto format:{DXGI_FORMAT_R16G16_FLOAT,DXGI_FORMAT_R32G32_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT}){
        d.Format=format;Check(InspectInputDescription(d,tag,out)&&out.format==format,"native motion format retained");
    }
    d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;Check(!InspectInputDescription(d,tag,out),"color cannot masquerade as motion");
    tag.type=0;Check(!InspectInputDescription(d,tag,out),"color cannot masquerade as depth");
    std::puts("PASS: production extent/format validation without allocations or GPU commands");
}

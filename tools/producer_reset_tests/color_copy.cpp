#include "Framegen/ColorCopy.hpp"
#include <iostream>
#include <stdexcept>
using namespace cvr::framegen;
void Check(bool b,const char* m){if(!b)throw std::runtime_error(m);}
int main()try{
    D3D12_RESOURCE_DESC s{};s.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    s.Width=1968;s.Height=2112;s.DepthOrArraySize=s.MipLevels=1;s.SampleDesc.Count=1;s.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    auto d=s;d.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS;
    Check(PlanColorCopy(s,d,false).kind==ColorCopyKind::Whole,"equal compatible textures not copied");
    d.Width=1960;
    Check(PlanColorCopy(s,d,false).kind==ColorCopyKind::Reject,"unknown world projection silently cropped");
    auto p=PlanColorCopy(s,d,true);
    Check(p.kind==ColorCopyKind::MenuCenter&&p.source.left==4&&p.source.right==1964&&p.source.bottom==2112,"PPD16 menu crop wrong");
    s.Width=d.Width=1600;s.Height=1728;d.Height=1724;p=PlanColorCopy(s,d,true);
    Check(p.kind==ColorCopyKind::MenuCenter&&p.source.top==2&&p.source.bottom==1726,"PPD13 vertical menu crop wrong");
    s.Width=976;d.Width=965;s.Height=d.Height=1040;p=PlanColorCopy(s,d,true);
    Check(p.kind==ColorCopyKind::MenuCenter&&p.source.left==5&&p.source.right==970,"odd alignment overlap wrong");
    d.Width=900;Check(PlanColorCopy(s,d,true).kind==ColorCopyKind::Reject,"large stale resize silently accepted");
    d=s;d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;Check(PlanColorCopy(s,d,true).kind==ColorCopyKind::Reject,"channel-incompatible copy accepted");
    d=s;d.Width=1000;Check(PlanColorCopy(s,d,true).kind==ColorCopyKind::Reject,"out-of-bounds source copy accepted");
    std::cout<<"PASS exact copy, PPD menu overlap, incompatible/stale/world rejection\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}

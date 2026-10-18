#pragma once
#include <d3d12.h>
#include <cstdint>

namespace cvr::framegen {
enum class ColorCopyKind {Reject,Whole,MenuCenter,MenuScale};
struct ColorCopyPlan {ColorCopyKind kind=ColorCopyKind::Reject;D3D12_BOX source{};};
inline unsigned CopyFormatFamily(DXGI_FORMAT f){
    switch(f){
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:return 1;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:case DXGI_FORMAT_B8G8R8A8_UNORM:case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:return 2;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:case DXGI_FORMAT_R16G16B16A16_FLOAT:return 3;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:case DXGI_FORMAT_R10G10B10A2_UNORM:return 4;
    default:return 0;
    }
}
inline ColorCopyPlan PlanColorCopy(const D3D12_RESOURCE_DESC& s,const D3D12_RESOURCE_DESC& d,bool menu){
    if(s.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d.Dimension!=s.Dimension||
       !s.Width||!s.Height||!d.Width||!d.Height||
       (s.Format!=d.Format&&(!CopyFormatFamily(s.Format)||CopyFormatFamily(s.Format)!=CopyFormatFamily(d.Format)))||s.DepthOrArraySize!=d.DepthOrArraySize||
       s.MipLevels!=d.MipLevels||s.SampleDesc.Count!=d.SampleDesc.Count||s.SampleDesc.Quality!=d.SampleDesc.Quality)return {};
    if(s.Width==d.Width&&s.Height==d.Height)return {ColorCopyKind::Whole,{}};
    // Coupled PPD rounds native dimensions to a 16-texel grid, whereas the
    // submitted eye rectangle follows its calibrated aspect. A mono menu can
    // retain pixel scale by copying the centered overlap. Never invent a world
    // projection or rescale a stale resource of an arbitrary different size.
    if(!menu||!CopyFormatFamily(s.Format)||s.Width>UINT32_MAX||d.Width>UINT32_MAX||
       s.SampleDesc.Count!=1||s.DepthOrArraySize!=1||s.MipLevels!=1)return {};
    // A calibrated world eye can be substantially narrower than the desktop
    // menu (observed 1920 -> 1782). Preserve all UI edges using a shader blit;
    // a crop or an empty layer is not a valid substitute for the full menu.
    if(s.Width<d.Width||s.Height<d.Height||s.Width-d.Width>15||s.Height-d.Height>15)
        return {ColorCopyKind::MenuScale,{0,0,0,UINT(s.Width),s.Height,1}};
    const auto x=UINT((s.Width-d.Width)/2),y=UINT((s.Height-d.Height)/2);
    return {ColorCopyKind::MenuCenter,{x,y,0,x+UINT(d.Width),y+d.Height,1}};
}
inline bool RecordColorCopy(ID3D12GraphicsCommandList* list,ID3D12Resource* source,ID3D12Resource* target,bool menu){
    if(!list||!source||!target)return false;
    const auto plan=PlanColorCopy(source->GetDesc(),target->GetDesc(),menu);
    if(plan.kind==ColorCopyKind::Reject||plan.kind==ColorCopyKind::MenuScale)return false;
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={target,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_DEST};
    list->ResourceBarrier(1,&b);
    if(plan.kind==ColorCopyKind::Whole)list->CopyResource(target,source);
    else {
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=source;dst.pResource=target;
        src.Type=dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst,0,0,0,&src,&plan.source);
    }
    b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;b.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
    list->ResourceBarrier(1,&b);return true;
}
}

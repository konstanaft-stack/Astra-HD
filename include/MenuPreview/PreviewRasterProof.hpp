#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <ostream>
#include <locale>
#include <limits>

namespace cvr::menu_preview {
// Value-only diagnostic data. No resources, locks, GPU calls or admission policy.
struct PreviewRasterProof {
    static constexpr uint32_t Limit=16;
    struct Viewport {float TopLeftX{},TopLeftY{},Width{},Height{},MinDepth{},MaxDepth{};};
    struct Scissor {int32_t left{},top{},right{},bottom{};};
    uint64_t generation{},drawOrder{},resource{};
    uint32_t viewportCount{},scissorCount{};
    bool frozen{},observationLost{},viewportObserved{},scissorObserved{},viewportKnown{},scissorKnown{};
    std::array<Viewport,Limit> viewports{};
    std::array<Scissor,Limit> scissors{};
    std::array<bool,Limit> viewportValid{},scissorValid{};
};
struct PreviewRasterState {
    PreviewRasterProof current{};
    void Reset()noexcept{current={};}
    template<class T> void Viewports(uint32_t count,const T* values)noexcept{
        current.viewportObserved=true;current.viewportCount=count;current.viewportKnown=count>0&&count<=PreviewRasterProof::Limit&&values;
        current.viewports={};current.viewportValid={};
        if(!values||count>PreviewRasterProof::Limit)return;
        for(uint32_t i=0;i<count;++i){const auto& v=values[i];auto& d=current.viewports[i];
            d={v.TopLeftX,v.TopLeftY,v.Width,v.Height,v.MinDepth,v.MaxDepth};
            current.viewportValid[i]=std::isfinite(d.TopLeftX)&&std::isfinite(d.TopLeftY)&&std::isfinite(d.Width)&&std::isfinite(d.Height)&&std::isfinite(d.MinDepth)&&std::isfinite(d.MaxDepth);
            current.viewportKnown&=current.viewportValid[i];
        }
    }
    template<class T> void Scissors(uint32_t count,const T* values)noexcept{
        current.scissorObserved=true;current.scissorCount=count;current.scissorKnown=count>0&&count<=PreviewRasterProof::Limit&&values;
        current.scissors={};current.scissorValid={};
        if(!values||count>PreviewRasterProof::Limit)return;
        for(uint32_t i=0;i<count;++i){const auto& v=values[i];current.scissors[i]={v.left,v.top,v.right,v.bottom};current.scissorValid[i]=true;}
    }
    PreviewRasterProof Freeze(uint64_t generation,uint64_t drawOrder,uint64_t resource,bool observationLost=false)const noexcept{
        auto value=observationLost?PreviewRasterProof{}:current;value.observationLost=observationLost;value.generation=generation;value.drawOrder=drawOrder;value.resource=resource;
        value.frozen=generation&&drawOrder&&resource;return value;
    }
};
inline void WritePreviewRasterProofJson(std::ostream& out,const PreviewRasterProof& p){
    out.imbue(std::locale::classic()); // JSON decimal point, independent of user locale.
    struct PrecisionGuard {std::ostream& stream;std::streamsize previous;~PrecisionGuard(){stream.precision(previous);}} precision{out,out.precision()};
    out.precision(std::numeric_limits<float>::max_digits10);
    out<<"{\"provenance\":\"actual_matched_Draw_return_not_Copy_latest_state\",\"frozen\":"<<(p.frozen?"true":"false")<<",\"observation_lost\":"<<(p.observationLost?"true":"false")
       <<",\"recording_generation\":"<<p.generation<<",\"draw_order\":"<<p.drawOrder<<",\"resource\":"<<p.resource
       <<",\"viewport_observed\":"<<(p.viewportObserved?"true":"false")<<",\"viewport_count\":"<<p.viewportCount
       <<",\"viewport_known\":"<<(p.viewportKnown?"true":"false")<<",\"viewports\":[";
    bool comma=false;for(uint32_t i=0;i<PreviewRasterProof::Limit;++i)if(p.viewportValid[i]){if(comma)out<<',';comma=true;const auto& v=p.viewports[i];
        out<<"{\"index\":"<<i<<",\"values\":["<<v.TopLeftX<<','<<v.TopLeftY<<','<<v.Width<<','<<v.Height<<','<<v.MinDepth<<','<<v.MaxDepth<<"]}";}
    out<<"],\"scissor_observed\":"<<(p.scissorObserved?"true":"false")<<",\"scissor_count\":"<<p.scissorCount
       <<",\"scissor_known\":"<<(p.scissorKnown?"true":"false")<<",\"scissors\":[";
    comma=false;for(uint32_t i=0;i<PreviewRasterProof::Limit;++i)if(p.scissorValid[i]){if(comma)out<<',';comma=true;const auto& v=p.scissors[i];
        out<<"{\"index\":"<<i<<",\"values\":["<<v.left<<','<<v.top<<','<<v.right<<','<<v.bottom<<"]}";}
    out<<"]}";
}
}

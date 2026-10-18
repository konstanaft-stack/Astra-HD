#pragma once
#include "Framegen/SrHistoryOwner.hpp"
#include <nvsdk_ngx_defs_dlssg.h>
#include <cstring>
namespace cvr::framegen::native_fg {
using Matrix=std::array<float,16>;
inline Matrix Multiply(const Matrix&a,const Matrix&b){Matrix out{};for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c){double v{};for(unsigned k=0;k<4;++k)v+=double(a[r*4+k])*b[k*4+c];out[r*4+c]=float(v);}return out;}
inline Matrix ReadMatrix(const CameraConstants&c,unsigned offset){Matrix out{};std::copy_n(c.values.data()+offset,16,out.data());return out;}
inline bool CopyMatrix(const void* pointer,float* out) noexcept {if(!pointer)return false;__try{std::memcpy(out,pointer,64);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
struct Parameters {
    ID3D12Resource *color{},*motion{},*depth{};bool motionVoid{},depthVoid{};
    unsigned reset{},count{},index{},cameraMotion{},inverted{},jittered{},mw{},mh{},dw{},dh{};
    float scaleX{},scaleY{},jitterX{},jitterY{},fov{};
    std::array<void*,4> pointers{};std::array<Matrix,4> matrices{};
    static constexpr const char* matrixKeys[]{"DLSSG.CameraViewToClip","DLSSG.ClipToCameraView","DLSSG.ClipToPrevClip","DLSSG.PrevClipToClip"};
    bool Read(const NVSDK_NGX_Parameter*p){if(!p)return false;bool ok=true;
        const auto get=[&](const char*name,auto&value){ok&=p->Get(name,&value)==NVSDK_NGX_Result_Success;};
        const auto resource=[&](const char*name,ID3D12Resource*&value,bool&untyped){void* v{};ID3D12Resource*t{};
            const bool a=p->Get(name,&v)==NVSDK_NGX_Result_Success,b=p->Get(name,&t)==NVSDK_NGX_Result_Success;
            if((!a&&!b)||(a&&b&&v!=t)){ok=false;return;}untyped=a;value=a?static_cast<ID3D12Resource*>(v):t;ok&=value!=nullptr;};
        bool colorVoid{};resource("DLSSG.Backbuffer",color,colorVoid);resource("DLSSG.MVecs",motion,motionVoid);resource("DLSSG.Depth",depth,depthVoid);
        get("DLSSG.Reset",reset);get("DLSSG.MultiFrameCount",count);get("DLSSG.MultiFrameIndex",index);
        get("DLSSG.CameraMotionIncluded",cameraMotion);get("DLSSG.DepthInverted",inverted);get("DLSSG.MvecJittered",jittered);
        get("DLSSG.MvecScaleX",scaleX);get("DLSSG.MvecScaleY",scaleY);get("DLSSG.JitterOffsetX",jitterX);get("DLSSG.JitterOffsetY",jitterY);get("DLSSG.CameraFOV",fov);
        get("DLSSG.MVecsSubrectWidth",mw);get("DLSSG.MVecsSubrectHeight",mh);get("DLSSG.DepthSubrectWidth",dw);get("DLSSG.DepthSubrectHeight",dh);
        for(const auto*key:{"DLSSG.MVecsSubrectBaseX","DLSSG.MVecsSubrectBaseY","DLSSG.DepthSubrectBaseX","DLSSG.DepthSubrectBaseY"}){unsigned value{};get(key,value);ok&=value==0;}
        for(unsigned i=0;i<4;++i){get(matrixKeys[i],pointers[i]);ok&=CopyMatrix(pointers[i],matrices[i].data());}
        return ok&&reset<=1&&count>=1&&count<=3&&index>=1&&index<=count;
    }
    bool Matches(const SrFrameGenerationGuide&g)const{
        const auto& s=g.history[2];const auto& n=s.ngx;const auto& c=s.original;
        const auto close=[](float a,float b){return std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<=2e-4f*(1.f+std::abs(b));};
        if(!close(scaleX,n.mvScale[0])||!close(scaleY,n.mvScale[1])||!close(jitterX,n.jitterPixels[0])||!close(jitterY,n.jitterPixels[1])||
           cameraMotion!=c.flags[1]||inverted!=c.flags[0]||jittered!=c.flags[6]||mw!=n.renderSubrect[0]||mh!=n.renderSubrect[1]||dw!=mw||dh!=mh)return false;
        const unsigned offsets[]{0,16,48,64};for(unsigned m=0;m<4;++m)for(unsigned i=0;i<16;++i)if(!c.valuePresent[offsets[m]+i]||!close(matrices[m][i],c.values[offsets[m]+i]))return false;
        const auto cd=color->GetDesc(),md=motion->GetDesc(),dd=depth->GetDesc();
        return cd.Width==n.outputSize[0]&&cd.Height==n.outputSize[1]&&md.Width==n.motionSize[0]&&md.Height==n.motionSize[1]&&
            dd.Width==n.depthSize[0]&&dd.Height==n.depthSize[1]&&md.Format==g.motion->GetDesc().Format;
    }
};
class Override {
    NVSDK_NGX_Parameter* p;const Parameters& saved;
    Matrix clipToPrevious,previousToClip;
    void Resource(const char*key,ID3D12Resource*r,bool untyped){if(untyped)p->Set(key,static_cast<void*>(r));else p->Set(key,r);}
public:
    Override(const NVSDK_NGX_Parameter*params,const Parameters&original,const SrFrameGenerationGuide&guide,unsigned reset,float fov)
        :p(const_cast<NVSDK_NGX_Parameter*>(params)),saved(original){
        clipToPrevious=Multiply(ReadMatrix(guide.history[2].original,48),ReadMatrix(guide.history[1].original,48));
        previousToClip=Multiply(ReadMatrix(guide.history[1].original,64),ReadMatrix(guide.history[2].original,64));
        Resource("DLSSG.MVecs",guide.motion.Get(),saved.motionVoid);Resource("DLSSG.Depth",guide.depth.Get(),saved.depthVoid);
        p->Set("DLSSG.Reset",reset);p->Set("DLSSG.CameraFOV",fov);
        p->Set("DLSSG.ClipToPrevClip",static_cast<void*>(clipToPrevious.data()));p->Set("DLSSG.PrevClipToClip",static_cast<void*>(previousToClip.data()));
    }
    ~Override(){Resource("DLSSG.MVecs",saved.motion,saved.motionVoid);Resource("DLSSG.Depth",saved.depth,saved.depthVoid);
        p->Set("DLSSG.Reset",saved.reset);p->Set("DLSSG.CameraFOV",saved.fov);
        p->Set("DLSSG.ClipToPrevClip",saved.pointers[2]);p->Set("DLSSG.PrevClipToClip",saved.pointers[3]);}
    Override(const Override&)=delete;Override&operator=(const Override&)=delete;
};
}

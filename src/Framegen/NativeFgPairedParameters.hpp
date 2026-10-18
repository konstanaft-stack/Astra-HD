#pragma once
#include "Framegen/Inputs.hpp"
#include <nvsdk_ngx.h>
#include <array>
#include <cmath>
namespace cvr::framegen::native_fg {
// Temporary, fully restored parameters for the companion camera. No MAIN
// depth, motion, HUD mask or camera matrix is borrowed by the second view.
class PairedOverride {
    NVSDK_NGX_Parameter* p;
    struct Resource {const char* key{};ID3D12Resource* old{};bool untyped{};};
    struct Scalar {const char* key{};float old{};};
    struct Integer {const char* key{};unsigned old{};};
    struct Pointer {const char* key{};void* old{};};
    std::array<Resource,12> resources{};unsigned nr{};
    std::array<Scalar,32> scalars{};unsigned nf{};
    std::array<Integer,20> integers{};unsigned ni{};
    std::array<Pointer,5> pointers{};unsigned np{};
    std::array<std::array<float,16>,5> matrices{};
    bool ok=true;
    const char* reason="ready";
    void Fail(const char*key){if(ok)reason=key;ok=false;}
    void R(const char* key,ID3D12Resource* value,bool required=true){
        void* v{};ID3D12Resource* t{};const bool a=p->Get(key,&v)==NVSDK_NGX_Result_Success,b=p->Get(key,&t)==NVSDK_NGX_Result_Success;
        if((!a&&!b)||(a&&b&&v!=t)){if(required)Fail(key);return;}
        resources[nr++]={key,a?static_cast<ID3D12Resource*>(v):t,a};
        if(a)p->Set(key,static_cast<void*>(value));else p->Set(key,value);
    }
    void F(const char*key,float value,bool required=true){float old{};
        if(!std::isfinite(value)||p->Get(key,&old)!=NVSDK_NGX_Result_Success){if(required)Fail(key);return;}
        scalars[nf++]={key,old};p->Set(key,value);
    }
    void U(const char*key,unsigned value,bool required=true){unsigned old{};
        if(p->Get(key,&old)!=NVSDK_NGX_Result_Success){if(required)Fail(key);return;}
        integers[ni++]={key,old};p->Set(key,value);
    }
public:
    PairedOverride(const NVSDK_NGX_Parameter* parameters,const Inputs& g,ID3D12Resource* color,
            ID3D12Resource* generated,ID3D12Resource* real,ID3D12Resource* disable,unsigned reset)
        :p(const_cast<NVSDK_NGX_Parameter*>(parameters)){
        if(!p||!color||!generated||!real||!disable||!g.depth||!g.motion||!g.submitted||!g.evaluated){Fail("resources");return;}
        const auto& c=g.camera.constants;
        if(c.source==CameraConstantsSource::None||c.version<1||c.version>2){Fail("camera-source");return;}
        if(!c.flagPresent.all()){Fail("camera-flags-missing");return;}
        if(c.flags[2]){Fail("motion-3d");return;}if(c.flags[4]){Fail("orthographic");return;}
        R("DLSSG.Backbuffer",color);R("DLSSG.MVecs",g.motion.Get());R("DLSSG.Depth",g.depth.Get());
        R("DLSSG.OutputInterpolated",generated);R("DLSSG.OutputReal",real);R("DLSSG.OutputDisableInterpolation",disable);
        // UIR is off in the verified native creation. Missing create-time
        // Always/Never flags mean these resources remain optional per NGX.
        R("DLSSG.HUDLess",nullptr,false);R("DLSSG.UI",nullptr,false);R("DLSSG.UIAlpha",nullptr,false);
        R("DLSSG.BidirectionalDistortionField",nullptr,false);
        U("DLSSG.Reset",reset);U("DLSSG.CameraMotionIncluded",c.flags[1]);U("DLSSG.DepthInverted",c.flags[0]);
        U("DLSSG.MvecJittered",c.flags[6]);U("DLSSG.MvecDilated",c.flags[5]);
        U("DLSSG.MVecsSubrectBaseX",0);U("DLSSG.MVecsSubrectBaseY",0);U("DLSSG.MVecsSubrectWidth",g.motionWidth);U("DLSSG.MVecsSubrectHeight",g.motionHeight);
        U("DLSSG.DepthSubrectBaseX",0);U("DLSSG.DepthSubrectBaseY",0);U("DLSSG.DepthSubrectWidth",g.width);U("DLSSG.DepthSubrectHeight",g.height);
        F("DLSSG.MvecScaleX",g.camera.motionScale[0]);F("DLSSG.MvecScaleY",g.camera.motionScale[1]);
        F("DLSSG.JitterOffsetX",g.camera.jitter[0]);F("DLSSG.JitterOffsetY",g.camera.jitter[1]);
        F("DLSSG.CameraFOV",g.camera.verticalFov);F("DLSSG.CameraAspectRatio",g.camera.aspect,false);
        F("DLSSG.CameraNear",g.camera.nearPlane,false);F("DLSSG.CameraFar",g.camera.farPlane,false);
        const char* vectorKeys[]{"DLSSG.CameraPosX","DLSSG.CameraPosY","DLSSG.CameraPosZ",
            "DLSSG.CameraUpX","DLSSG.CameraUpY","DLSSG.CameraUpZ","DLSSG.CameraRightX","DLSSG.CameraRightY","DLSSG.CameraRightZ",
            "DLSSG.CameraFwdX","DLSSG.CameraFwdY","DLSSG.CameraFwdZ"};
        for(unsigned i=0;i<12;++i)F(vectorKeys[i],c.values[86+i],false);
        // Geometry validation already rejects nonzero pinhole offsets; invalid
        // optional SL values mean the SDK's default centred pinhole.
        F("DLSSG.CameraPinholeOffsetX",0,false);F("DLSSG.CameraPinholeOffsetY",0,false);
        const char* keys[]{"DLSSG.CameraViewToClip","DLSSG.ClipToCameraView","DLSSG.ClipToLensClip","DLSSG.ClipToPrevClip","DLSSG.PrevClipToClip"};
        for(unsigned m=0;m<5;++m){void* old{};
            if(p->Get(keys[m],&old)!=NVSDK_NGX_Result_Success){if(m!=2)Fail(keys[m]);continue;}
            bool known=true;for(unsigned i=0;i<16;++i){const auto index=m*16+i;known&=c.valuePresent.test(index)&&std::isfinite(c.values[index])&&std::abs(c.values[index])<1e30f;matrices[m][i]=c.values[index];}
            if(!known&&m!=2){Fail(keys[m]);continue;}
            pointers[np++]={keys[m],old};p->Set(keys[m],known?static_cast<void*>(matrices[m].data()):nullptr);
        }
    }
    ~PairedOverride(){if(!p)return;
        for(unsigned i=np;i>0;--i)p->Set(pointers[i-1].key,pointers[i-1].old);
        for(unsigned i=ni;i>0;--i)p->Set(integers[i-1].key,integers[i-1].old);
        for(unsigned i=nf;i>0;--i)p->Set(scalars[i-1].key,scalars[i-1].old);
        for(unsigned i=nr;i>0;--i){const auto&r=resources[i-1];if(r.untyped)p->Set(r.key,static_cast<void*>(r.old));else p->Set(r.key,r.old);}
    }
    explicit operator bool()const{return ok;}
    const char* Reason()const{return reason;}
    PairedOverride(const PairedOverride&)=delete;PairedOverride&operator=(const PairedOverride&)=delete;
};
}

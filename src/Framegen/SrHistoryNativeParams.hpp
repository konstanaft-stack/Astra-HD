#pragma once
#include <nvsdk_ngx.h>
#include <cmath>

namespace cvr::framegen::sr_owner {
// Every overwritten field is first obtained with its exact public NGX type.
// This never retains the borrowed parameter object beyond one native call.
struct NativeParameters {
    ID3D12Resource* color{};ID3D12Resource* depth{};ID3D12Resource* motion{};ID3D12Resource* output{};
    ID3D12Resource* specular{};bool specularKnown{};
    bool colorVoid{},depthVoid{},motionVoid{},outputVoid{},specularVoid{};
    int reset{};float delta{},jitterX{},jitterY{},scaleX{},scaleY{};bool deltaKnown{};
    unsigned width{},height{},colorX{},colorY{},depthX{},depthY{},motionX{},motionY{},outputX{},outputY{};
    uint64_t missing{};
    bool Read(const NVSDK_NGX_Parameter*p){if(!p){missing=UINT64_MAX;return false;}unsigned bit=0;
        const auto get=[&](const char*name,auto&v){if(p->Get(name,&v)!=NVSDK_NGX_Result_Success)missing|=uint64_t(1)<<bit;++bit;};
        const auto resource=[&](const char*name,ID3D12Resource*&v,bool&isVoid){void*untyped{};ID3D12Resource*typed{};
            const bool a=p->Get(name,&untyped)==NVSDK_NGX_Result_Success,b=p->Get(name,&typed)==NVSDK_NGX_Result_Success;
            // SL's public path stores native resources as void*. Prefer that
            // observed representation; support typed-only public parameter bags.
            if(a&&(untyped||!b||!typed)){v=static_cast<ID3D12Resource*>(untyped);isVoid=true;}
            else if(b){v=typed;isVoid=false;}else{missing|=uint64_t(1)<<bit;}++bit;};
        resource(NVSDK_NGX_Parameter_Color,color,colorVoid);resource(NVSDK_NGX_Parameter_Depth,depth,depthVoid);
        resource(NVSDK_NGX_Parameter_MotionVectors,motion,motionVoid);resource(NVSDK_NGX_Parameter_Output,output,outputVoid);
        get(NVSDK_NGX_Parameter_Reset,reset);
        // Luke 42DA: 270146 Get(float), 27019D skips Set/cache update unless
        // success. Missing time is permitted; keep bit 5 reserved for telemetry.
        deltaKnown=p->Get(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec,&delta)==NVSDK_NGX_Result_Success;++bit;
        if(!deltaKnown)delta=0;
        get(NVSDK_NGX_Parameter_Jitter_Offset_X,jitterX);get(NVSDK_NGX_Parameter_Jitter_Offset_Y,jitterY);
        get(NVSDK_NGX_Parameter_MV_Scale_X,scaleX);get(NVSDK_NGX_Parameter_MV_Scale_Y,scaleY);
        get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,width);get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,height);
        get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,colorX);get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y,colorY);
        get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,depthX);get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y,depthY);
        get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,motionX);get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y,motionY);
        get(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X,outputX);get(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y,outputY);
        const auto before=missing;resource(NVSDK_NGX_Parameter_MotionVectorsReflection,specular,specularVoid);specularKnown=missing==before;missing=before;
        const auto valid=[](float v){return std::isfinite(v)&&std::abs(v)<3.4e38f;};
        return !missing&&color&&depth&&motion&&output&&(reset==0||reset==1)&&(!deltaKnown||(valid(delta)&&delta>=0))&&
            valid(jitterX)&&valid(jitterY)&&valid(scaleX)&&valid(scaleY)&&scaleX!=0&&scaleY!=0&&width&&height;
    }
};
class ParameterOverride {
public:
    ParameterOverride(const NVSDK_NGX_Parameter*p,const NativeParameters&original):parameters(const_cast<NVSDK_NGX_Parameter*>(p)),saved(original){}
    ~ParameterOverride(){if(color)Resource(NVSDK_NGX_Parameter_Color,saved.color,saved.colorVoid);
        if(specular)Resource(NVSDK_NGX_Parameter_MotionVectorsReflection,saved.specular,saved.specularVoid);
        if(time)parameters->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec,saved.delta);
        if(reset)parameters->Set(NVSDK_NGX_Parameter_Reset,saved.reset);
        if(motion)Resource(NVSDK_NGX_Parameter_MotionVectors,saved.motion,saved.motionVoid);}
    ParameterOverride(const ParameterOverride&)=delete;ParameterOverride&operator=(const ParameterOverride&)=delete;
    void Motion(ID3D12Resource*v){motion=true;Resource(NVSDK_NGX_Parameter_MotionVectors,v,saved.motionVoid);}
    void Reset(int v){reset=true;parameters->Set(NVSDK_NGX_Parameter_Reset,v);}
    void Delta(float v){if(saved.deltaKnown){time=true;parameters->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec,v);}}
    void Specular(ID3D12Resource*v){specular=true;Resource(NVSDK_NGX_Parameter_MotionVectorsReflection,v,saved.specularVoid);}
    void Color(ID3D12Resource*v){color=true;Resource(NVSDK_NGX_Parameter_Color,v,saved.colorVoid);}
private:
    void Resource(const char*name,ID3D12Resource*value,bool asVoid){if(asVoid)parameters->Set(name,static_cast<void*>(value));else parameters->Set(name,value);}
    NVSDK_NGX_Parameter*parameters;NativeParameters saved;bool color{},specular{},time{},reset{},motion{};
};
}

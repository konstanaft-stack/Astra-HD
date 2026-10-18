#pragma once
#include "Framegen/SrHistoryOwner.hpp"
#include <cstring>
#include <vector>
struct OwnerParameters:NVSDK_NGX_Parameter {
    ID3D12Resource *color{},*depth{},*motion{},*output{},*specular{};int reset{},flags=10;
    float delta=16,jitterX{},jitterY{},scaleX=32,scaleY=24;unsigned width=32,height=24;
    bool voidResources{},deltaPresent=true;unsigned deltaSets{};
    void Set(const char*,unsigned long long)override{}void Set(const char*,double)override{}void Set(const char*,unsigned int)override{}
    void Set(const char*n,float v)override{if(!strcmp(n,NVSDK_NGX_Parameter_FrameTimeDeltaInMsec)){delta=v;deltaPresent=true;++deltaSets;}}
    void Set(const char*n,int v)override{if(!strcmp(n,NVSDK_NGX_Parameter_Reset))reset=v;}
    void Set(const char*,ID3D11Resource*)override{}void Set(const char*n,void*v)override{if(voidResources)SetResource(n,static_cast<ID3D12Resource*>(v));}
    void Set(const char*n,ID3D12Resource*v)override{if(!voidResources)SetResource(n,v);}
    void SetResource(const char*n,ID3D12Resource*v){if(!strcmp(n,NVSDK_NGX_Parameter_MotionVectors))motion=v;else if(!strcmp(n,NVSDK_NGX_Parameter_Color))color=v;else if(!strcmp(n,NVSDK_NGX_Parameter_MotionVectorsReflection))specular=v;}
    void Reset()override{}
    NVSDK_NGX_Result Get(const char*,unsigned long long*)const override{return NVSDK_NGX_Result_FAIL_InvalidParameter;}
    NVSDK_NGX_Result Get(const char*,double*)const override{return NVSDK_NGX_Result_FAIL_InvalidParameter;}
    NVSDK_NGX_Result Get(const char*,ID3D11Resource**)const override{return NVSDK_NGX_Result_FAIL_InvalidParameter;}
    NVSDK_NGX_Result Get(const char*n,void**v)const override{if(!voidResources)return NVSDK_NGX_Result_FAIL_InvalidParameter;ID3D12Resource*r{};const auto result=GetResource(n,&r);*v=r;return result;}
    NVSDK_NGX_Result Get(const char*n,int*v)const override{if(!strcmp(n,NVSDK_NGX_Parameter_Reset))*v=reset;else if(!strcmp(n,NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags))*v=flags;else return NVSDK_NGX_Result_FAIL_InvalidParameter;return NVSDK_NGX_Result_Success;}
    NVSDK_NGX_Result Get(const char*n,unsigned*v)const override{if(!strcmp(n,NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width))*v=width;
        else if(!strcmp(n,NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height))*v=height;else if(std::strstr(n,"Subrect.Base."))*v=0;else return NVSDK_NGX_Result_FAIL_InvalidParameter;return NVSDK_NGX_Result_Success;}
    NVSDK_NGX_Result Get(const char*n,float*v)const override{if(!strcmp(n,NVSDK_NGX_Parameter_FrameTimeDeltaInMsec)){if(!deltaPresent)return NVSDK_NGX_Result_FAIL_UnsupportedParameter;*v=delta;}
        else if(!strcmp(n,NVSDK_NGX_Parameter_Jitter_Offset_X))*v=jitterX;else if(!strcmp(n,NVSDK_NGX_Parameter_Jitter_Offset_Y))*v=jitterY;
        else if(!strcmp(n,NVSDK_NGX_Parameter_MV_Scale_X))*v=scaleX;else if(!strcmp(n,NVSDK_NGX_Parameter_MV_Scale_Y))*v=scaleY;else return NVSDK_NGX_Result_FAIL_InvalidParameter;return NVSDK_NGX_Result_Success;}
    NVSDK_NGX_Result Get(const char*n,ID3D12Resource**v)const override{if(voidResources)return NVSDK_NGX_Result_FAIL_InvalidParameter;return GetResource(n,v);}
    NVSDK_NGX_Result GetResource(const char*n,ID3D12Resource**v)const{if(!strcmp(n,NVSDK_NGX_Parameter_Color))*v=color;else if(!strcmp(n,NVSDK_NGX_Parameter_Depth))*v=depth;
        else if(!strcmp(n,NVSDK_NGX_Parameter_MotionVectors))*v=motion;else if(!strcmp(n,NVSDK_NGX_Parameter_Output))*v=output;
        else if(!strcmp(n,NVSDK_NGX_Parameter_MotionVectorsReflection))*v=specular;else return NVSDK_NGX_Result_FAIL_InvalidParameter;return NVSDK_NGX_Result_Success;}
};
struct OwnerCall {uintptr_t handle{};int reset{};float delta{};bool deltaKnown{};ID3D12Resource*motion{};};
inline unsigned ownerCreates{};inline std::vector<OwnerCall> ownerCalls;
inline std::function<void(ID3D12GraphicsCommandList*,ID3D12Resource*)> ownerConsumer;
inline NVSDK_NGX_Result NVSDK_CONV OwnerCreate(ID3D12GraphicsCommandList*,NVSDK_NGX_Feature,NVSDK_NGX_Parameter*,NVSDK_NGX_Handle**h){*h=reinterpret_cast<NVSDK_NGX_Handle*>(uintptr_t(++ownerCreates==1?0x1110:0x2220));return NVSDK_NGX_Result_Success;}
inline NVSDK_NGX_Result NVSDK_CONV OwnerEvaluate(ID3D12GraphicsCommandList*l,const NVSDK_NGX_Handle*h,const NVSDK_NGX_Parameter*p,PFN_NVSDK_NGX_ProgressCallback){OwnerCall call;call.handle=reinterpret_cast<uintptr_t>(h);
    p->Get(NVSDK_NGX_Parameter_Reset,&call.reset);call.deltaKnown=p->Get(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec,&call.delta)==NVSDK_NGX_Result_Success;
    void* untyped{};if(p->Get(NVSDK_NGX_Parameter_MotionVectors,&untyped)==NVSDK_NGX_Result_Success)call.motion=static_cast<ID3D12Resource*>(untyped);else p->Get(NVSDK_NGX_Parameter_MotionVectors,&call.motion);ownerCalls.push_back(call);
    if(ownerConsumer)ownerConsumer(l,call.motion);return NVSDK_NGX_Result_Success;}
inline NVSDK_NGX_Result NVSDK_CONV OwnerRelease(NVSDK_NGX_Handle*){return NVSDK_NGX_Result_Success;}
inline cvr::framegen::SrNativeApiContext OwnerContext(){cvr::framegen::SrNativeApiContext c;c.originals={OwnerCreate,OwnerEvaluate,OwnerRelease};c.coreModule=0x1000;c.moduleGeneration=2;c.lifecycleGeneration=3;c.createOrdinal=4;c.featureId=1;c.featureKnown=c.modulePinned=true;return c;}

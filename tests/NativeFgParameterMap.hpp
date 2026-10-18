#pragma once
#include <unordered_map>
#include <string>
struct FgParameters:NVSDK_NGX_Parameter {
    std::unordered_map<std::string,unsigned> integers;
    std::unordered_map<std::string,float> floats;
    std::unordered_map<std::string,void*> pointers;
    std::array<std::array<float,16>,5> matrices{};
    FgParameters(){
        integers={{"DLSSG.Reset",0},{"DLSSG.MultiFrameCount",1},{"DLSSG.MultiFrameIndex",1},
            {"DLSSG.CameraMotionIncluded",1},{"DLSSG.DepthInverted",1},{"DLSSG.MvecJittered",0},
            {"DLSSG.MVecsSubrectBaseX",0},{"DLSSG.MVecsSubrectBaseY",0},{"DLSSG.MVecsSubrectWidth",32},{"DLSSG.MVecsSubrectHeight",24},
            {"DLSSG.DepthSubrectBaseX",0},{"DLSSG.DepthSubrectBaseY",0},{"DLSSG.DepthSubrectWidth",32},{"DLSSG.DepthSubrectHeight",24}};
        floats={{"DLSSG.MvecScaleX",32.f},{"DLSSG.MvecScaleY",24.f},{"DLSSG.JitterOffsetX",0.f},{"DLSSG.JitterOffsetY",0.f},{"DLSSG.CameraFOV",90.f}};
    }
    void Set(const char* n,unsigned long long v)override{integers[n]=unsigned(v);}void Set(const char* n,double v)override{floats[n]=float(v);}
    void Set(const char* n,unsigned v)override{integers[n]=v;}void Set(const char*n,int v)override{integers[n]=unsigned(v);}
    void Set(const char*n,float v)override{floats[n]=v;}
    void Set(const char*n,void*v)override{pointers[n]=v;}void Set(const char*n,ID3D12Resource*v)override{pointers[n]=v;}void Set(const char*n,ID3D11Resource*v)override{pointers[n]=v;}
    NVSDK_NGX_Result Get(const char*n,unsigned*v)const override{const auto i=integers.find(n);if(i==integers.end())return NVSDK_NGX_Result_FAIL_InvalidParameter;*v=i->second;return NVSDK_NGX_Result_Success;}
    NVSDK_NGX_Result Get(const char*n,int*v)const override{unsigned u{};const auto r=Get(n,&u);*v=int(u);return r;}
    NVSDK_NGX_Result Get(const char*n,unsigned long long*v)const override{unsigned u{};const auto r=Get(n,&u);*v=u;return r;}
    NVSDK_NGX_Result Get(const char*n,float*v)const override{const auto i=floats.find(n);if(i==floats.end())return NVSDK_NGX_Result_FAIL_InvalidParameter;*v=i->second;return NVSDK_NGX_Result_Success;}
    NVSDK_NGX_Result Get(const char*n,double*v)const override{float f{};const auto r=Get(n,&f);*v=f;return r;}
    NVSDK_NGX_Result Get(const char*n,void**v)const override{const auto i=pointers.find(n);if(i==pointers.end())return NVSDK_NGX_Result_FAIL_InvalidParameter;*v=i->second;return NVSDK_NGX_Result_Success;}
    NVSDK_NGX_Result Get(const char*n,ID3D12Resource**v)const override{void*p{};const auto r=Get(n,&p);*v=static_cast<ID3D12Resource*>(p);return r;}
    NVSDK_NGX_Result Get(const char*,ID3D11Resource**)const override{return NVSDK_NGX_Result_FAIL_InvalidParameter;}
    void Reset()override{}
};

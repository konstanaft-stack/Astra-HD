#pragma once
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <string>

namespace cvr::ppd {
constexpr int MinWidth=768, MaxWidth=6000, Alignment=8;
constexpr double Pi=3.14159265358979323846;
// Same central angular-sampling convention as cpvr::plan_resolution in our
// existing core. Quest3's current ToFu render covers108deg, not its94deg span.
constexpr double Quest3ReferenceFov=108.0;
struct Resolution {int width{},height{};bool valid{};};
inline bool ValidFov(double fov){return std::isfinite(fov) && fov>1 && fov<170;}
inline double CenterPpd(int width,double fov){
    if(width<=0 || !ValidFov(fov))return 0;
    return width*(Pi/180.0)/(2*std::tan(fov*Pi/360.0));
}
inline Resolution ForWidth(int width){
    if(width<MinWidth || width>MaxWidth || width%Alignment)return {};
    // Integer HALF-UP must match the staged author asset generator exactly.
    return {width,((width*2296+2064*4)/(2064*8))*8,true};
}
inline Resolution FromPpd(double ppd,double fov){
    if(!std::isfinite(ppd) || ppd<5 || ppd>50 || !ValidFov(fov))return {};
    const double raw=ppd*180.0/Pi*(2*std::tan(fov*Pi/360.0));
    if(!std::isfinite(raw) || raw<MinWidth || raw>MaxWidth)return {};
    return ForWidth(static_cast<int>(std::lround(raw/Alignment))*Alignment);
}
inline int MinTick(double fov){return std::max(50,static_cast<int>(std::ceil(CenterPpd(MinWidth,fov)*10)));}
inline int MaxTick(double fov){return std::min(500,static_cast<int>(std::floor(CenterPpd(MaxWidth,fov)*10)));}
inline bool ParsePpd(const wchar_t* text,double& value){
    if(!text)return false;
    std::wstring copy(text);
    std::replace(copy.begin(),copy.end(),L',',L'.');
    wchar_t* end=nullptr;
    const double parsed=std::wcstod(copy.c_str(),&end);
    if(end==copy.c_str() || !std::isfinite(parsed))return false;
    while(*end && std::iswspace(*end))++end;
    if(*end)return false;
    value=parsed;return true;
}
}

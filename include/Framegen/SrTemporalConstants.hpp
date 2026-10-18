#pragma once
// Validated CPU builder promoted without changing the reference algebra.
// No game pointers, NGX calls, XR loop, resource creation,
// history queue or GPU ownership. Caller supplies actual, coherent samples.
#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace cvr::framegen::sr {
using V2=std::array<float,2>;using V3=std::array<float,3>;using V4=std::array<float,4>;
using M4=std::array<float,16>;using U2=std::array<uint32_t,2>;
struct alignas(16) Constants {
    M4 transf0{},transf_2{};V4 camMoComp_2{};
    V2 mvScale0{},mvOffset0{},mvScale_1{},mvOffset_1{},mvInvScale0{},mvInvOffset0{};
    V2 uvJitter0{},uvJitter_1{},uvJitter_2{},mv2depthScale{},dso0{},dso1{},dso2{},dso3{};
    U2 motionMapSize{};V2 mvSize{},mvSizeRcp{},uvMax{},srcSubrectSize{},srcSubrectSizeRcp{},src2OutRatio{},src2OutMVRatio{};
    V3 uvw2xyzScale0{};float zNear0{};V3 uvw2xyzOffset0{};float zFar0{};
    V3 uvw2xyzScale_1{};float zNear_1{};V3 uvw2xyzOffset_1{};float zFar_1{};
    V3 xyz2uvwScale_2{};float zNear_2{};V3 xyz2uvwOffset_2{};float zFar_2{};
    float minEstimatedZ{},offsetZ{};uint32_t miscFlags{},compatFlags{},debugFlags{};int32_t debugInt{};
    float debugFloat{},pad0{},depthTolerance{},mvTolerancePixels{},staticThreshold{},staticZExp{},mcDepthTolerance{};V3 pad1{};
};
static_assert(sizeof(Constants)==480);
static_assert(offsetof(Constants,camMoComp_2)==128&&offsetof(Constants,mvScale0)==144);
static_assert(offsetof(Constants,uvJitter0)==192&&offsetof(Constants,motionMapSize)==256);
static_assert(offsetof(Constants,uvw2xyzScale0)==320&&offsetof(Constants,xyz2uvwScale_2)==384);
static_assert(offsetof(Constants,miscFlags)==424&&offsetof(Constants,mcDepthTolerance)==464);
struct Pose {V4 orientation{0,0,0,1};V3 position{};};
struct Epochs {
    uint64_t renderer{},gameWorld{},hmdWorld{},resource{};
    bool operator==(const Epochs&)const=default;
    bool Valid()const{return renderer&&gameWorld&&hmdWorld&&resource;}
};
struct Sample {
    int64_t sourceId=-1,matrixSourceId=-1;
    uint64_t featureKey{},basisId{},parametersKey{};Epochs epochs;
    Pose camera;
    std::optional<Pose> explicitReference;
    std::array<Pose,2> eyeFromReference{};bool eyeOffsetsKnown{};
    V4 projection{};float nearZ{},farZ{};
    V2 jitterPixels{},mvScale{};
    U2 depthSize{},motionSize{},renderSubrect{},outputSize{};
    // specularMvPresent attests BOTH source and prepared fix output resources.
    bool lowResMv{},jitteredMv{},beginsNewHistory{},specularMvPresent{};
};
struct Parameters {
    bool known{},effectiveFix{};uint64_t key{};
    uint32_t ngxFeature{}; // verified SuperSampling=1 or RayReconstruction=13
    uint32_t compatFlags{},debugFlags{};int32_t debugInt{};float debugFloat{},pad0{};V3 pad1{};
    bool useActualParity{},swapEyeOffsets{},linearDepth{};int32_t depthMode{};
    V2 mvMultiplier{},projectionGain{},projectionOffset{};
    float depthScale{};std::optional<V2> nearFarOverride;
    float minEstimatedZ{},offsetZ{},depthTolerance{},mvTolerancePixels{},staticThreshold{},staticZExp{},mcDepthTolerance{};
};
struct History {Sample current;std::optional<Sample> previous,previousPrevious;};
enum class Status {Ready,Disabled,UnknownParameters,UnsupportedFeature,NeedHistory,HistoryMismatch,InvalidSample,InvalidParameters};
struct Result {Status status=Status::NeedHistory;std::array<uint32_t,3> reserved{};Constants constants{};};
namespace detail {
template<class T>using Vec3=std::array<T,3>;
template<class T>using Quat=std::array<T,4>;
template<class T>using Mat=std::array<T,16>;
template<class T>struct Rigid {Quat<T> q;Vec3<T> p;};
template<class T>Vec3<T> Rotate(const Quat<T>&q,const Vec3<T>&v){
    const Vec3<T> t{T(2)*(q[1]*v[2]-q[2]*v[1]),T(2)*(q[2]*v[0]-q[0]*v[2]),T(2)*(q[0]*v[1]-q[1]*v[0])};
    return {(t[0]*q[3]+v[0]+t[2]*q[1])-t[1]*q[2],
            (t[1]*q[3]+v[1]+t[0]*q[2])-t[2]*q[0],
            (t[2]*q[3]+v[2]+t[1]*q[0])-t[0]*q[1]};
}
template<class T>Rigid<T> InversePose(const Rigid<T>& a){
    const Quat<T> q{-a.q[0],-a.q[1],-a.q[2],a.q[3]};
    return {q,Rotate(q,Vec3<T>{-a.p[0],-a.p[1],-a.p[2]})};
}
template<class T>Rigid<T> ConvertPose(const Pose& source){
    Rigid<T> out;for(unsigned i=0;i<4;++i)out.q[i]=T(source.orientation[i]);
    for(unsigned i=0;i<3;++i)out.p[i]=T(source.position[i]);
    if constexpr(sizeof(T)==sizeof(double)){
        const auto&q=out.q;const T norm=std::sqrt(((q[0]*q[0]+q[1]*q[1])+q[2]*q[2])+q[3]*q[3]);
        for(auto& component:out.q)component*=T(1)/norm;
    }
    return out;
}
inline Pose InvertRawFloat(const Pose& source){
    const auto inverse=InversePose(ConvertPose<float>(source));return {inverse.q,inverse.p};
}
template<class T>Rigid<T> Compose(const Rigid<T>&a,const Rigid<T>&b){
    const auto&x=a.q;const auto&y=b.q;const auto t=Rotate(x,b.p);
    return {{x[0]*y[3]+y[0]*x[3]+x[1]*y[2]-x[2]*y[1],
             y[1]*x[3]-y[2]*x[0]+x[1]*y[3]+x[2]*y[0],
             y[1]*x[0]+y[2]*x[3]-x[1]*y[0]+x[2]*y[3],
             y[3]*x[3]-x[0]*y[0]-x[1]*y[1]-x[2]*y[2]},
            {t[0]+a.p[0],t[1]+a.p[1],t[2]+a.p[2]}};
}
template<class T>Mat<T> PoseMatrix(const Rigid<T>&a){
    const T x=a.q[0],y=a.q[1],z=a.q[2],w=a.q[3];
    return {((x*x+w*w)-y*y)-z*z,T(2)*(x*y-z*w),T(2)*(x*z+y*w),a.p[0],
            T(2)*(x*y+z*w),((w*w-x*x)+y*y)-z*z,T(2)*(y*z-x*w),a.p[1],
            T(2)*(x*z-y*w),T(2)*(y*z+x*w),((w*w-x*x)-y*y)+z*z,a.p[2],T(0),T(0),T(0),T(1)};
}
template<class T>Mat<T> InverseMatrix(const Mat<T>&a){
    Mat<T> out{};out[15]=T(1);
    for(unsigned r=0;r<3;++r){for(unsigned c=0;c<3;++c)out[r*4+c]=a[c*4+r];
        // Same rigid transpose(R) * translate(-p) operation as 2B9B20.
        out[r*4+3]=((out[r*4+1]*-a[7]+out[r*4]*-a[3])+out[r*4+2]*-a[11]);}
    return out;
}
template<class T>Mat<T> Relative(const Mat<T>& inversePrevious,const Mat<T>&current){
    Mat<T> out{};out[15]=T(1);
    for(unsigned r=0;r<3;++r){for(unsigned c=0;c<3;++c){
        const T first=inversePrevious[r*4]*current[c],second=inversePrevious[r*4+1]*current[4+c],third=inversePrevious[r*4+2]*current[8+c];
        // 2B9590/2B97A0 group the first two terms by output column.
        out[r*4+c]=(c==0?first+second:second+first)+third;
    }
    out[r*4+3]=((inversePrevious[r*4+3]+inversePrevious[r*4]*current[3])+inversePrevious[r*4+1]*current[7])+inversePrevious[r*4+2]*current[11];}
    return out;
}
template<class T>Vec3<T> ReferencePosition(const Sample&s,const Parameters&p){
    Rigid<T> h;
    if(s.explicitReference)h=ConvertPose<T>(*s.explicitReference);
    else{
        const auto id=p.useActualParity?s.sourceId:s.matrixSourceId;
        const unsigned eye=unsigned(uint64_t(id)&1u)^unsigned(p.swapEyeOffsets);
        h=Compose(ConvertPose<T>(s.camera),InversePose(ConvertPose<T>(s.eyeFromReference[eye])));
    }
    if(p.debugFlags&0x20000u)h=InversePose(h);
    return h.p;
}
template<class T>void Matrices(Constants&out,const Sample&c,const Sample&m,const Sample&o,const Parameters&p){
    const bool invert=(p.debugFlags&0x20000u)!=0;
    const auto camera=[&](const Sample&s){return PoseMatrix(ConvertPose<T>(invert?InvertRawFloat(s.camera):s.camera));};
    const auto v0=camera(c),v1=camera(m),v2=camera(o),i1=InverseMatrix(v1),i2=InverseMatrix(v2);
    const auto t0=Relative(i1,v0),t2=Relative(i2,v1);
    // Row-major bytes are deliberate: the original DXIL dots p with each
    // stored float4. Do not apply the VS warp's extra packing transpose here.
    for(unsigned i=0;i<16;++i){out.transf0[i]=float(t0[i]);out.transf_2[i]=float(t2[i]);}
    const auto h0=ReferencePosition<T>(c,p),h1=ReferencePosition<T>(m,p),h2=ReferencePosition<T>(o,p);
    const Vec3<T> delta{(h2[0]+h0[0])-(h1[0]+h1[0]),(h2[1]+h0[1])-(h1[1]+h1[1]),(h2[2]+h0[2])-(h1[2]+h1[2])};
    for(unsigned r=0;r<3;++r)out.camMoComp_2[r]=float((delta[0]*i2[r*4]+delta[1]*i2[r*4+1])+delta[2]*i2[r*4+2]);
    out.camMoComp_2[3]=0;
}
template<size_t N>bool Finite(const std::array<float,N>&v){return std::all_of(v.begin(),v.end(),[](float x){return std::isfinite(x);});}
inline bool ValidPose(const Pose&p){
    if(!Finite(p.orientation)||!Finite(p.position))return false;
    double length=0;for(float q:p.orientation)length+=double(q)*double(q);return length>1e-30&&std::isfinite(length);
}
inline bool ValidSize(const U2&s){return s[0]&&s[1]&&s[0]<=INT32_MAX&&s[1]<=INT32_MAX;}
inline bool ValidSample(const Sample&s){
    if(s.sourceId<0||s.matrixSourceId<0||!s.featureKey||!s.basisId||!s.parametersKey||!s.epochs.Valid()||
       !ValidPose(s.camera)||!Finite(s.projection)||!Finite(s.jitterPixels)||!Finite(s.mvScale)||
       !ValidSize(s.depthSize)||!ValidSize(s.motionSize)||!ValidSize(s.renderSubrect)||!ValidSize(s.outputSize)||
       !(s.nearZ>0)||!std::isfinite(s.nearZ)||!(s.farZ>s.nearZ))return false;
    if(s.explicitReference)return ValidPose(*s.explicitReference);
    return s.eyeOffsetsKnown&&ValidPose(s.eyeFromReference[0])&&ValidPose(s.eyeFromReference[1]);
}
inline bool SameHistory(const Sample& newer,const Sample& older){
    return newer.sourceId-older.sourceId==1&&newer.matrixSourceId-older.matrixSourceId==1&&
        newer.featureKey==older.featureKey&&newer.basisId==older.basisId&&newer.parametersKey==older.parametersKey&&
        newer.epochs==older.epochs&&newer.depthSize==older.depthSize&&newer.motionSize==older.motionSize&&
        newer.outputSize==older.outputSize&&!newer.beginsNewHistory;
}
struct ScalarState {V2 jitter{},mvScale{},uvMax{};V3 scale{},offset{};float nearZ{},farZ{};};
inline ScalarState Scalars(const Sample&s,const Parameters&p){
    ScalarState v;for(unsigned i=0;i<2;++i){const float depth=float(s.depthSize[i]),mv=float(s.motionSize[i]),rect=float(s.renderSubrect[i]);
        v.uvMax[i]=s.lowResMv?rect/depth:1.f;v.jitter[i]=(1.f/(s.lowResMv?depth:rect))*s.jitterPixels[i];
        v.mvScale[i]=(p.mvMultiplier[i]*s.mvScale[i])*(1.f/mv);}
    v.nearZ=s.nearZ;v.farZ=s.farZ;
    if(p.nearFarOverride&&(*p.nearFarOverride)[0]>=0&&(*p.nearFarOverride)[1]>0){v.nearZ=(*p.nearFarOverride)[0];v.farZ=(*p.nearFarOverride)[1];}
    float zScale=1,zOffset=0;
    if(!p.linearDepth&&p.depthMode==0){zScale=v.nearZ/v.farZ-1.f;zOffset=1.f;}
    else if(!p.linearDepth&&p.depthMode==1){zScale=1.f-v.nearZ/v.farZ;zOffset=v.nearZ/v.farZ;}
    v.scale={((s.projection[2]+s.projection[3])*p.projectionGain[0])/v.uvMax[0],
             -((s.projection[0]+s.projection[1])*p.projectionGain[1])/v.uvMax[1],zScale*p.depthScale};
    v.offset={-s.projection[2]-p.projectionOffset[0],p.projectionOffset[1]+s.projection[0],zOffset};return v;
}
inline bool ValidScalars(const ScalarState&v){
    return Finite(v.jitter)&&Finite(v.mvScale)&&Finite(v.uvMax)&&Finite(v.scale)&&Finite(v.offset)&&
        v.mvScale[0]!=0&&v.mvScale[1]!=0&&v.scale[0]!=0&&v.scale[1]!=0&&v.scale[2]!=0&&std::isfinite(v.nearZ)&&v.nearZ>=0&&v.farZ>v.nearZ;
}
}
inline Result Build(const History&h,const Parameters&p){
    Result out;const auto fail=[&](Status status){out.status=status;return out;};
    if(!p.effectiveFix)return fail(Status::Disabled);
    if(!p.known||!p.key)return fail(Status::UnknownParameters);
    if(p.ngxFeature!=1&&p.ngxFeature!=13)return fail(Status::UnsupportedFeature);
    if(!h.previous||!h.previousPrevious)return fail(Status::NeedHistory);
    const auto&c=h.current;const auto&m=*h.previous;const auto&o=*h.previousPrevious;
    if(!detail::ValidSample(c)||!detail::ValidSample(m)||!detail::ValidSample(o))return fail(Status::InvalidSample);
    if(c.parametersKey!=p.key||!detail::SameHistory(c,m)||!detail::SameHistory(m,o))return fail(Status::HistoryMismatch);
    const std::array<float,9> controls{p.debugFloat,p.pad0,p.depthScale,p.minEstimatedZ,p.offsetZ,p.depthTolerance,p.mvTolerancePixels,p.staticThreshold,p.staticZExp};
    if(!detail::Finite(controls)||!detail::Finite(p.pad1)||!detail::Finite(p.mvMultiplier)||!detail::Finite(p.projectionGain)||
       !detail::Finite(p.projectionOffset)||!std::isfinite(p.mcDepthTolerance))return fail(Status::InvalidParameters);
    const auto a=detail::Scalars(c,p),b=detail::Scalars(m,p),d=detail::Scalars(o,p);
    if(!detail::ValidScalars(a)||!detail::ValidScalars(b)||!detail::ValidScalars(d))return fail(Status::InvalidParameters);
    auto&k=out.constants;
    if(p.debugFlags&0x10000u)detail::Matrices<float>(k,c,m,o,p);else detail::Matrices<double>(k,c,m,o,p);
    if(!detail::Finite(k.transf0)||!detail::Finite(k.transf_2)||!detail::Finite(k.camMoComp_2))return fail(Status::InvalidSample);
    k.uvJitter0=a.jitter;k.uvJitter_1=b.jitter;k.uvJitter_2=d.jitter;k.mvScale0=a.mvScale;k.mvScale_1=b.mvScale;
    k.motionMapSize=c.lowResMv?c.renderSubrect:c.motionSize;k.uvMax=a.uvMax;
    for(unsigned i=0;i<2;++i){
        k.mvOffset0[i]=c.jitteredMv?a.jitter[i]-b.jitter[i]:0.f;
        k.mvOffset_1[i]=m.jitteredMv?b.jitter[i]-d.jitter[i]:0.f;
        k.mvInvScale0[i]=1.f/a.mvScale[i];k.mvInvOffset0[i]=c.jitteredMv?(d.jitter[i]-a.jitter[i])*k.mvInvScale0[i]:0.f;
        k.mv2depthScale[i]=c.lowResMv?1.f:float(c.renderSubrect[i])/float(c.depthSize[i]);
        k.mvSize[i]=float(c.motionSize[i]);k.mvSizeRcp[i]=1.f/k.mvSize[i];
        k.srcSubrectSize[i]=float(c.renderSubrect[i]);k.srcSubrectSizeRcp[i]=1.f/k.srcSubrectSize[i];
        k.src2OutRatio[i]=float(c.outputSize[i])*k.srcSubrectSizeRcp[i];
        const float half=.5f/float(c.depthSize[i]);k.dso0[i]=-half;k.dso3[i]=half;
        k.dso1[i]=i==0?half:-half;k.dso2[i]=i==0?-half:half;
    }
    k.uvw2xyzScale0=a.scale;k.uvw2xyzOffset0=a.offset;k.zNear0=a.nearZ;k.zFar0=a.farZ;
    k.uvw2xyzScale_1=b.scale;k.uvw2xyzOffset_1=b.offset;k.zNear_1=b.nearZ;k.zFar_1=b.farZ;
    for(unsigned i=0;i<3;++i){k.xyz2uvwScale_2[i]=1.f/d.scale[i];k.xyz2uvwOffset_2[i]=-d.offset[i]*k.xyz2uvwScale_2[i];}
    k.zNear_2=d.nearZ;k.zFar_2=d.farZ;k.minEstimatedZ=p.minEstimatedZ;k.offsetZ=p.offsetZ;
    k.miscFlags=uint32_t(uint64_t(c.sourceId)&1u)|(c.lowResMv?2u:0u)|(c.specularMvPresent?4u:0u)|(p.linearDepth?8u:0u);
    k.compatFlags=p.compatFlags;k.debugFlags=p.debugFlags;k.debugInt=p.debugInt;k.debugFloat=p.debugFloat;k.pad0=p.pad0;k.pad1=p.pad1;
    k.depthTolerance=p.depthTolerance;k.mvTolerancePixels=p.mvTolerancePixels;k.staticThreshold=p.staticThreshold;k.staticZExp=p.staticZExp;k.mcDepthTolerance=p.mcDepthTolerance;
    out.status=Status::Ready;return out;
}
} // namespace cvr::framegen::sr

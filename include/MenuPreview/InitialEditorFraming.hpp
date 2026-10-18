#pragma once
#include <array>
#include <cmath>
#include <cstdint>
namespace cvr::menu_preview {
enum class InitialFramingResult : uint32_t {Rejected=0,Ready=1,Pending=2};
// Scalar-only lifetime state. No entity, renderer or resource ownership.
struct InitialEditorFraming {
    uint32_t epoch{}; uint64_t controller{},actor{},actorID{},owner{};
    std::array<double,3> centre{}; double actorYaw{},delta{}; bool oriented{};
    void Clear() noexcept {*this={};}
    bool latched{};uint32_t sampleFrame{},sampleCount{};
    std::array<double,3> samplePosition{},sampleStaging{};std::array<double,4> sampleRotation{};
    InitialFramingResult Publish(uint32_t e,uint64_t c,uint64_t a,uint64_t aid,uint64_t player,uint64_t o,
                 const std::array<double,3>& p,const std::array<double,4>& q,
                 const std::array<double,3>& staging,uint32_t frame,bool frameKnown) noexcept {
        using R=InitialFramingResult;
        if(!e||!c||!a||!aid||(player&&a==player)||!o)return R::Rejected;
        if(epoch&&(epoch!=e||controller!=c||actor!=a||actorID!=aid||owner!=o))return R::Rejected;
        if(latched)return R::Ready; // Same identity; category placeholders/QE must never re-anchor.
        if(!epoch){epoch=e;controller=c;actor=a;actorID=aid;owner=o;}
        const auto pending=[&]{sampleCount=0;return R::Pending;};
        for(double v:p)if(!std::isfinite(v))return pending();
        for(double v:staging)if(!std::isfinite(v))return pending();
        double n=0;for(double v:q){if(!std::isfinite(v))return pending();n+=v*v;}
        if(n<.5||n>1.5||std::hypot(p[0]-staging[0],p[1]-staging[1])>2.0||std::abs(p[2]-staging[2])>4.0||
           std::hypot(staging[0],staging[1])<1.0||!frameKnown)return pending();
        if(sampleCount&&int32_t(frame-sampleFrame)<=0)return R::Pending;
        bool stable=sampleCount!=0;
        for(unsigned i=0;i<3;++i)stable=stable&&std::abs(p[i]-samplePosition[i])<=.01&&std::abs(staging[i]-sampleStaging[i])<=.01;
        double dot=0,priorNorm=0;for(unsigned i=0;i<4;++i){dot+=q[i]*sampleRotation[i];priorNorm+=sampleRotation[i]*sampleRotation[i];}
        // <=0.1 degree orientation change, quaternion sign invariant.
        stable=stable&&priorNorm>0&&std::abs(dot)/std::sqrt(n*priorNorm)>=.9999996192282494;
        sampleCount=stable?sampleCount+1:1;sampleFrame=frame;samplePosition=p;sampleStaging=staging;sampleRotation=q;
        if(sampleCount<3)return R::Pending;
        centre=p;actorYaw=std::atan2(2*(q[3]*q[2]+q[0]*q[1]),n-2*(q[1]*q[1]+q[2]*q[2]));latched=true;return R::Ready;
    }
    bool Orbit(uint32_t e,uint64_t c,uint64_t o,double radians,std::array<double,3>& p,std::array<double,4>& q)const noexcept {
        if(!latched||epoch!=e||controller!=c||owner!=o||!std::isfinite(radians))return false;
        if(radians==0)return true; // Exact identity, no extra rounding/normalization.
        for(double v:p)if(!std::isfinite(v))return false;
        double n=0;for(double v:q){if(!std::isfinite(v))return false;n+=v*v;}if(n<.5||n>1.5)return false;
        const double cs=std::cos(radians),sn=std::sin(radians),x=p[0]-centre[0],y=p[1]-centre[1];
        p[0]=centre[0]+cs*x-sn*y;p[1]=centre[1]+sn*x+cs*y;
        const double s=std::sin(radians/2),c0=std::cos(radians/2);const auto v=q;
        q={c0*v[0]-s*v[1],c0*v[1]+s*v[0],c0*v[2]+s*v[3],c0*v[3]-s*v[2]};return true;
    }
    bool Apply(uint32_t e,uint64_t c,uint64_t o,std::array<double,3>& p,std::array<double,4>& q) noexcept {
        if(!latched||!epoch||epoch!=e||controller!=c||owner!=o)return false;
        double n=0;for(double v:q){if(!std::isfinite(v))return false;n+=v*v;}
        if(n<.5||n>1.5)return false;for(double v:p)if(!std::isfinite(v))return false;
        if(!oriented){const double yaw=std::atan2(2*(q[3]*q[2]+q[0]*q[1]),n-2*(q[1]*q[1]+q[2]*q[2]));
            delta=std::remainder(actorYaw+3.14159265358979323846-yaw,2*3.14159265358979323846);oriented=true;}
        const double cs=std::cos(delta),sn=std::sin(delta),x=p[0]-centre[0],y=p[1]-centre[1];
        p[0]=centre[0]+cs*x-sn*y;p[1]=centre[1]+sn*x+cs*y;
        const double s=std::sin(delta/2),c0=std::cos(delta/2);
        const auto v=q;q={c0*v[0]-s*v[1],c0*v[1]+s*v[0],c0*v[2]+s*v[3],c0*v[3]-s*v[2]};return true;
    }
};
}

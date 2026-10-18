#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace cvr::framegen {
struct SourceDisplaySubmission {bool accepted{};int64_t requestedDisplayTimeNs{};std::array<int64_t,2> sourceIds{-1,-1};uint32_t eyeCount=2;};
// Our optional NVIDIA path may display a proven temporal position between two
// real AER sources. This is NOT an integer source identity or a Luke FG claim.
struct SourceDisplayContinuousSubmission {bool accepted{};int64_t requestedDisplayTimeNs{};std::array<double,2> sourcePositions{-1,-1};uint32_t eyeCount=2;};
enum class SourceDisplayStatus {Observed,NotAccepted,NoSources,InvalidInput};
struct SourceDisplayObservation {SourceDisplayStatus status=SourceDisplayStatus::InvalidInput;uint32_t addedSamples{};};
struct SourceDisplayFit {bool valid{};uint64_t samples{};double secondsPerSourceId{},offsetSeconds{};};

// Single-owner prediction clock, separate from the runtime QPC SourceClock.
// X=drawn source identity, Y=requested display time of an accepted XR submit.
// Repeated IDs and the two eyes at one display time are separate samples.
// Units of the slope are seconds/source ID, not measured FPS or GPU duration.
// Luke42DA: 29E574..29E5B8 -> 2A5780..2A5A8F; prediction at2A2BD0.
// No timestamps, poses, fences, XR calls or cap settings are manufactured here.
class SourceDisplayClock {
public:
    static constexpr int64_t MaxExactSourceId=(int64_t{1}<<53)-1;

    // Caller invokes only after all actual copy/signal/EndFrame acceptance
    // checks. A failed submission, malformed pair, invalid time or clock
    // rewind is not partially sampled. -1 means no drawn source for an eye.
    SourceDisplayObservation ObserveAccepted(const SourceDisplaySubmission& value) noexcept {
        if(!value.accepted)return {SourceDisplayStatus::NotAccepted,0};
        if(value.eyeCount<1 || value.eyeCount>2 || value.requestedDisplayTimeNs<=0 ||
           (lastDisplayTime && value.requestedDisplayTimeNs<lastDisplayTime))return {};
        uint32_t count=0;
        for(uint32_t eye=0;eye<value.eyeCount;++eye) {
            const auto id=value.sourceIds[eye];
            if(id< -1 || id>MaxExactSourceId)return {};
            if(id>=0)++count;
        }
        if(!count)return {SourceDisplayStatus::NoSources,0};
        const double time=static_cast<double>(value.requestedDisplayTimeNs)*1e-9;
        for(uint32_t eye=0;eye<value.eyeCount;++eye)
            if(value.sourceIds[eye]>=0)Add(static_cast<double>(value.sourceIds[eye]),time);
        lastDisplayTime=value.requestedDisplayTimeNs;
        return {SourceDisplayStatus::Observed,count};
    }
    SourceDisplayObservation ObserveAcceptedContinuous(const SourceDisplayContinuousSubmission& value) noexcept {
        if(!value.accepted)return {SourceDisplayStatus::NotAccepted,0};
        if(value.eyeCount<1 || value.eyeCount>2 || value.requestedDisplayTimeNs<=0 ||
           (lastDisplayTime && value.requestedDisplayTimeNs<lastDisplayTime))return {};
        uint32_t count{};
        for(uint32_t eye=0;eye<value.eyeCount;++eye) {
            const double x=value.sourcePositions[eye];
            if(!std::isfinite(x) || (x<0 && x!=-1) || x>double(MaxExactSourceId))return {};
            if(x>=0)++count;
        }
        if(!count)return {SourceDisplayStatus::NoSources,0};
        const double time=static_cast<double>(value.requestedDisplayTimeNs)*1e-9;
        for(uint32_t eye=0;eye<value.eyeCount;++eye)
            if(value.sourcePositions[eye]>=0)Add(value.sourcePositions[eye],time);
        lastDisplayTime=value.requestedDisplayTimeNs;
        return {SourceDisplayStatus::Observed,count};
    }
    SourceDisplayFit Fit() const noexcept {return {valid,totalSamples,slope,intercept};}
    std::optional<int64_t> PredictTimeNs(int64_t sourceId) const noexcept {
        if(!valid || sourceId<0 || sourceId>MaxExactSourceId)return {};
        const double seconds=static_cast<double>(sourceId)*slope+intercept;
        const double ns=seconds*1e9;
        // INT64_MAX rounds to 2^63 as double: use the exclusive exact limit
        // before converting, so an out-of-range float->integer cast cannot occur.
        if(!std::isfinite(ns) || ns<1.0 || ns>=0x1p63)return {};
        return static_cast<int64_t>(ns);
    }
    void Reset() noexcept {
        banks={};active=cursor=warmup=0;totalSamples=0;
        slope=intercept=0.0;valid=false;lastDisplayTime=0;
        // Reference reset leaves sample arrays but clears both accumulators;
        // the fresh bank must collect100 new samples before validity returns.
        if(generation!=(std::numeric_limits<uint64_t>::max)())++generation;
    }
    uint64_t Generation() const noexcept {return generation;}
private:
    struct Bank {double originX{},originY{},meanX{},meanY{},variance{},covariance{};};
    void Add(double x,double y) noexcept {
        const auto i=cursor;
        const auto other=1u-active;
        const auto oldX=xs[i],oldY=ys[i];xs[i]=x;ys[i]=y;
        auto& a=banks[active];auto& b=banks[other];
        if(i==0){b.originX=x;b.originY=y;}
        const auto oldMeanX=a.meanX,oldMeanY=a.meanY;
        const auto deltaMeanX=(x-oldX)*0.01;
        a.meanX=oldMeanX+deltaMeanX;
        a.meanY=(y-oldY)*0.01+a.meanY;
        const auto newCenteredX=(x-a.originX)-a.meanX;
        const auto oldCenteredX=(oldX-a.originX)-oldMeanX;
        a.variance=(oldCenteredX+newCenteredX)*deltaMeanX+a.variance;
        a.covariance=(((y-a.originY)-a.meanY)*newCenteredX-
                     ((oldY-a.originY)-oldMeanY)*oldCenteredX)*(1.0/99.0)+a.covariance;
        const auto dx=(x-b.originX)-b.meanX,dy=(y-b.originY)-b.meanY;
        const auto inv=1.0/static_cast<double>(i+1);
        const auto dm=dx*inv;
        b.meanX=b.meanX+dm;b.meanY=dy*inv+b.meanY;
        b.variance=(dm*dx+b.variance)*static_cast<double>(i)*inv;
        b.covariance=(dm*dy+b.covariance)*static_cast<double>(i)*inv;
        if(i+1==100){banks[active]={};active=other;cursor=0;}
        else ++cursor;
        if(totalSamples!=(std::numeric_limits<uint64_t>::max)())++totalSamples;
        if(warmup<100)++warmup;
        if(warmup==100) {
            const auto& current=banks[active];
            slope=current.variance!=0.0 ? current.covariance/current.variance : 0.0;
            intercept=(current.originY+current.meanY)-(current.originX+current.meanX)*slope;
            // Preserve reference's zero-variance slope0/valid=true behavior.
            // Conversion guards belong to PredictTimeNs, not a guessed cap.
            valid=true;
        }
    }
    std::array<double,100> xs{},ys{};
    std::array<Bank,2> banks{};
    uint32_t active{},cursor{},warmup{};
    uint64_t totalSamples{},generation=1;
    int64_t lastDisplayTime{};
    double slope{},intercept{};
    bool valid{};
};
}

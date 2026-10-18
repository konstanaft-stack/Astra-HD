#include <Windows.h> // Actual game include context: do not rely on NOMINMAX.
#include "Framegen/AerV2Policy.hpp"
#include "Framegen/SourceDisplayClock.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace cvr::framegen;
static void Check(bool ok,const char* why) {if(!ok)throw std::runtime_error(why);}
static bool Near(double a,double b,double absolute=1e-10,double relative=1e-9) {
    return std::abs(a-b)<=absolute+relative*(std::max)(std::abs(a),std::abs(b));
}
static AerV2Admission Ready(AerV2Eye eye,int64_t source) {return {eye,source,true};}
static void Window(const AerV2Request& r,AerV2Eye eye,int64_t low,int64_t high) {
    Check(r.eye==eye && r.lowerSource==low && r.upperSource==high,"source window/eye differs from reference");
}
static void ColdSequentialReread() {
    AerV2Policy p;std::vector<AerV2Request> calls;
    auto result=p.Step(0,0,-1,[&](const AerV2Request& r)->std::optional<AerV2Admission> {
        calls.push_back(r);
        return Ready(r.eye,r.eye==AerV2Eye::Right ? 3 : 2);
    });
    Check(result.status==AerV2StepStatus::Advanced && calls.size()==2,"cold frame did not run two consumers");
    Window(calls[0],AerV2Eye::Right,1,INT64_MAX);
    Window(calls[1],AerV2Eye::Left,2,4); // Must re-read accepted right=3, not initial right=-1.
    Check(p.State().sourceIds==std::array<int64_t,2>{2,3},"accepted eye IDs were merged or lost");
    Check(p.State().acceptedFrames==std::array<int64_t,2>{0,0},"source ID was used as compositor anchor");
    Check(result.delayForDraw==1 && result.nextDelay==1,"zero applied delay did not map to next D1");
    calls.clear();
    p.Step(1,0,999,[&](const AerV2Request& r)->std::optional<AerV2Admission> {calls.push_back(r);return {};});
    Window(calls[0],AerV2Eye::Right,5,INT64_MAX);
    Window(calls[1],AerV2Eye::Left,4,4); // D1 ignores U=999.
}
static void ReadinessAndIdentity() {
    for(int bad=0;bad<5;++bad) {
        AerV2Policy p;std::vector<AerV2Request> calls;
        p.Step(0,0,-1,[&](const AerV2Request& r)->std::optional<AerV2Admission> {
            calls.push_back(r);
            if(r.eye==AerV2Eye::Left)return {};
            if(bad==0)return AerV2Admission{r.eye,1,false};
            if(bad==1)return Ready(AerV2Eye::Left,1);
            if(bad==2)return Ready(r.eye,2); // wrong parity for the producer's explicit ID domain
            if(bad==3)return Ready(r.eye,-1);
            return Ready(r.eye,INT64_MAX); // no room for next signed +2
        });
        Check(calls.size()==2,"rejected first candidate lost second consumer attempt");
        Window(calls[1],AerV2Eye::Left,0,0);
        Check(p.State().sourceIds==std::array<int64_t,2>{-2,-1},"unready or foreign source changed admission state");
        Check(p.State().acceptedFrames==std::array<int64_t,2>{-6,-6},"rejected source advanced a compositor anchor");
    }
}
static void ColdLeftOnlyAndUpperBound() {
    AerV2Policy p;int calls=0;
    p.Step(0,0,-1,[&](const AerV2Request& r)->std::optional<AerV2Admission> {
        ++calls;return r.eye==AerV2Eye::Left ? std::optional{Ready(r.eye,2)} : std::nullopt;
    });
    Check(calls==2 && p.State().sourceIds[0]==-2,"above-upper source became accepted in cold left window");
    p.Step(1,0,-1,[](const AerV2Request& r)->std::optional<AerV2Admission> {
        return r.eye==AerV2Eye::Left ? std::optional{Ready(r.eye,0)} : std::nullopt;
    });
    Check(p.State().sourceIds[0]==0 && p.State().sourceIds[1]==-1,"available cold left source was not independently admitted");
}
static void DelayedAgeAndReread() {
    AerV2Policy p;Check(p.SetPriorFixedDelay(4),"known fixed fraction was not accepted");
    std::vector<AerV2Request> calls;
    auto f0=p.Step(0,4,-1,[&](const AerV2Request& r)->std::optional<AerV2Admission> {calls.push_back(r);return Ready(r.eye,0);});
    Check(calls.size()==1 && f0.delayForDraw==4,"right age test ignored newly accepted left anchor");
    Window(calls[0],AerV2Eye::Left,0,0);
    calls.clear();p.Step(1,4,7,[&](const AerV2Request& r)->std::optional<AerV2Admission> {calls.push_back(r);return {};});
    Check(calls.empty(),"D4 age boundary admitted one frame too early");
    auto f2=p.Step(2,4,7,[&](const AerV2Request& r)->std::optional<AerV2Admission> {calls.push_back(r);return Ready(r.eye,3);});
    Check(calls.size()==1 && f2.accepted[1],"right source was not admitted on exact half-delay boundary");
    Window(calls[0],AerV2Eye::Right,1,7);
    Check(p.State().sourceIds==std::array<int64_t,2>{0,3},"D>1 merged source and anchor domains");
    Check(p.State().acceptedFrames==std::array<int64_t,2>{0,2},"D>1 anchor was not compositor F");
}
static void DelayedRejectedFirstAndOddHalf() {
    AerV2Policy p;Check(p.SetPriorFixedDelay(3),"D3 seed failed");std::vector<AerV2Request> calls;
    p.Step(0,3,-1,[&](const AerV2Request& r)->std::optional<AerV2Admission> {calls.push_back(r);return {};});
    Check(calls.size()==2,"failed left consume incorrectly suppressed right attempt");
    Window(calls[0],AerV2Eye::Left,0,0);Window(calls[1],AerV2Eye::Right,-1,-1);
    p.Step(1,3,0,[](const AerV2Request& r)->std::optional<AerV2Admission> {return r.eye==AerV2Eye::Left ? std::optional{Ready(r.eye,0)} : std::nullopt;});
    calls.clear();p.Step(2,3,1,[&](const AerV2Request& r)->std::optional<AerV2Admission> {calls.push_back(r);return Ready(r.eye,1);});
    Check(calls.size()==1 && calls[0].eye==AerV2Eye::Right,"odd D did not use integer truncation for half delay");
}
static void FixedToModeZeroTransient() {
    AerV2Policy p;Check(p.SetPriorFixedDelay(6),"D6 fixed seed failed");
    auto first=p.Step(0,0,-1,[](const AerV2Request&)->std::optional<AerV2Admission>{return {};});
    Check(first.delayForDraw==6 && first.nextDelay==1,"mode0 erased inherited D before its draw or retained it indefinitely");
    std::vector<AerV2Request> calls;
    auto next=p.Step(1,0,-1,[&](const AerV2Request& r)->std::optional<AerV2Admission>{calls.push_back(r);return {};});
    Check(next.delayForDraw==1 && calls.size()==2 && calls[0].eye==AerV2Eye::Right,"mode0 did not settle to default after transient");
    Check(!p.SetPriorFixedDelay(1) && !p.SetPriorFixedDelay(7),"unproved fixed denominator was accepted");
}
static void InvalidFramesAndReset() {
    AerV2Policy p;int calls=0;
    auto none=[&](const AerV2Request&)->std::optional<AerV2Admission>{++calls;return {};};
    Check(p.Step(-1,0,-1,none).status==AerV2StepStatus::InvalidInput,"negative compositor F was accepted");
    Check(p.Step(0,-1,-1,none).status==AerV2StepStatus::InvalidInput,"negative applied delay was guessed/clamped");
    Check(p.Step(0,0,-2,none).status==AerV2StepStatus::InvalidInput,"foreign U sentinel accepted");
    Check(calls==0,"invalid frame invoked external admission");
    p.Step(9,0,-1,none);calls=0;
    Check(p.Step(9,0,-1,none).status==AerV2StepStatus::InvalidInput && calls==0,"duplicate compositor tick reconsumed sources");
    Check(p.Step(8,0,-1,none).status==AerV2StepStatus::InvalidInput,"rewound compositor F did not require reset");
    p.Reset();auto s=p.State();
    Check(s.sourceIds==std::array<int64_t,2>{-2,-1} && s.acceptedFrames==std::array<int64_t,2>{-6,-6},"reset left stale history");
    Check(p.Step(0,0,-1,none).status==AerV2StepStatus::Advanced,"reset failed to start a new counter domain");
}
static void SignedAgeOverflow() {
    AerV2Policy p;Check(p.SetPriorFixedDelay(6),"seed failed");std::vector<AerV2Request> calls;
    p.Step(INT64_MAX-1,6,-1,[&](const AerV2Request& r)->std::optional<AerV2Admission>{calls.push_back(r);return r.eye==AerV2Eye::Left ? std::optional{Ready(r.eye,0)} : std::nullopt;});
    Check(calls.size()==1,"cold negative anchor age overflowed near int64 max");
    calls.clear();p.Step(INT64_MAX,6,-1,[&](const AerV2Request& r)->std::optional<AerV2Admission>{calls.push_back(r);return {};});
    Check(calls.empty(),"anchor+D overflow made an unready age appear eligible");
}

static SourceDisplaySubmission Sample(int64_t source,int64_t time,bool accepted=true) {
    return {accepted,time,{source,-1},1};
}
static void ClockThresholdAndSavedGolden() {
    SourceDisplayClock c;
    for(int64_t i=0;i<99;++i)Check(c.ObserveAccepted(Sample(i,100'000'000'000+i*10'000'000)).addedSamples==1,"valid submission was not sampled");
    Check(!c.Fit().valid && !c.PredictTimeNs(100),"clock became valid before sample100");
    c.ObserveAccepted(Sample(99,100'990'000'000));
    Check(c.Fit().valid && Near(c.Fit().secondsPerSourceId,.01,1e-13,1e-12),"affine source clock differs at sample100");
    Check(c.PredictTimeNs(100) && std::abs(*c.PredictTimeNs(100)-101'000'000'000LL)<=2,"fit predicted the wrong requested time");
    c.Reset();
    // Literal reference values from source-policy-20261004/clock-reference-vectors.json.
    for(int64_t i=0;i<200;++i) {
        const auto ns=int64_t(std::llround((100.0+double(i)/90.0)*1e9));
        c.ObserveAccepted(Sample(i,ns));
        if(i==99)Check(Near(c.Fit().secondsPerSourceId,0.011111111111111113,1e-11,1e-10) && Near(c.Fit().offsetSeconds,100.0,2e-9,0),"saved 100-sample golden differs");
        if(i==199)Check(Near(c.Fit().secondsPerSourceId,0.01111111111111111,1e-11,1e-10),"saved bank-rotation golden differs");
    }
}
static void IndependentSlidingOls() {
    SourceDisplayClock c;std::vector<std::pair<double,double>> data;
    for(int64_t i=0;i<500;++i) {
        const auto x=i*i;
        const auto ns=int64_t(std::llround((100.0+double(i)/90.0+std::sin(double(i))*.001)*1e9));
        Check(c.ObserveAccepted(Sample(x,ns)).addedSamples==1,"nonlinear sample was dropped");
        data.emplace_back(double(x),double(ns)*1e-9);
        if(i<99)continue;
        long double mx=0,my=0;const auto start=data.size()-100;
        for(size_t j=start;j<data.size();++j){mx+=data[j].first;my+=data[j].second;}mx/=100;my/=100;
        long double vx=0,xy=0;
        for(size_t j=start;j<data.size();++j){const auto dx=static_cast<long double>(data[j].first)-mx;vx+=dx*dx;xy+=dx*(static_cast<long double>(data[j].second)-my);}
        const auto a=xy/vx,b=my-a*mx;
        Check(Near(c.Fit().secondsPerSourceId,double(a),1e-14,1e-9),"sliding estimate differs from independent last100 OLS");
        Check(Near(c.Fit().offsetSeconds,double(b),1e-9,1e-11),"sliding intercept retained expired samples");
    }
}
static void AcceptedOnlyAndRepeats() {
    SourceDisplayClock c;
    for(int i=0;i<80;++i)c.ObserveAccepted(Sample(i,300'000'000'000+int64_t(i)*1'000'000,false));
    Check(c.Fit().samples==0,"failed EndFrame polluted the clock window");
    for(int64_t i=0;i<50;++i) {
        SourceDisplaySubmission pair{true,300'000'000'000+i*10'000'000,{7,7},2};
        Check(c.ObserveAccepted(pair).addedSamples==2,"repeated source ID was filtered or eyes merged");
    }
    const auto f=c.Fit();
    Check(f.samples==100 && f.valid && f.secondsPerSourceId==0.0,"zero-variance source window changed reference validity");
    Check(Near(f.offsetSeconds,300.245,1e-10,1e-12),"repeat time average differs");
    Check(c.PredictTimeNs(123).has_value(),"zero-slope reference estimate was silently replaced by a limiter");
}
static void ClockResetAndEpoch() {
    SourceDisplayClock c;const auto before=c.Generation();
    for(int64_t i=0;i<120;++i)c.ObserveAccepted(Sample(i,400'000'000'000+i*10'000'000));
    c.Reset();Check(c.Generation()!=before && !c.Fit().valid && c.Fit().samples==0,"reset retained fit validity/count/epoch");
    for(int64_t i=0;i<99;++i)c.ObserveAccepted(Sample(10000+i,500'000'000'000+i*20'000'000));
    Check(!c.Fit().valid,"retained ring samples shortened reset warmup");
    c.ObserveAccepted(Sample(10099,501'980'000'000));
    Check(Near(c.Fit().secondsPerSourceId,.02,1e-12,1e-11) && Near(c.Fit().offsetSeconds,300.0,1e-9,1e-11),"reset fit mixed old and new clock domains");
}
static void ClockInvalidAndOverflow() {
    SourceDisplayClock c;
    auto bad=c.ObserveAccepted(Sample(-2,100));Check(bad.status==SourceDisplayStatus::InvalidInput && c.Fit().samples==0,"invalid signed ID entered fit");
    Check(c.ObserveAccepted(Sample(INT64_MAX,100)).status==SourceDisplayStatus::InvalidInput,"non-exact double source ID was accepted");
    Check(c.ObserveAccepted(Sample(0,0)).status==SourceDisplayStatus::InvalidInput,"missing requested time entered fit");
    SourceDisplaySubmission malformed{true,100,{0,-2},2};c.ObserveAccepted(malformed);
    Check(c.Fit().samples==0,"part of an invalid pair was committed");
    c.ObserveAccepted(Sample(0,1000));c.ObserveAccepted(Sample(1,999));
    Check(c.Fit().samples==1,"rewound display time did not require reset");
    c.Reset();
    for(int64_t i=0;i<100;++i)c.ObserveAccepted(Sample(i,INT64_MAX/2+i*1'000'000));
    Check(!c.PredictTimeNs(-1) && !c.PredictTimeNs(INT64_MAX),"invalid prediction ID was converted");
    Check(!c.PredictTimeNs(SourceDisplayClock::MaxExactSourceId),"predicted nanoseconds overflowed int64");
}
int main() {
    int failed=0,passed=0;
    const std::pair<const char*,void(*)()> cases[]={
      {"policy_cold_reread",ColdSequentialReread},{"policy_readiness_identity",ReadinessAndIdentity},
      {"policy_cold_left_upper",ColdLeftOnlyAndUpperBound},{"policy_delayed_age_reread",DelayedAgeAndReread},
      {"policy_delayed_reject_odd",DelayedRejectedFirstAndOddHalf},{"policy_fixed_transient",FixedToModeZeroTransient},
      {"policy_invalid_reset",InvalidFramesAndReset},{"policy_signed_overflow",SignedAgeOverflow},
      {"clock_threshold_golden",ClockThresholdAndSavedGolden},{"clock_independent_ols",IndependentSlidingOls},
      {"clock_accepted_repeats",AcceptedOnlyAndRepeats},{"clock_reset_epoch",ClockResetAndEpoch},
      {"clock_invalid_overflow",ClockInvalidAndOverflow}};
    for(const auto& [name,test]:cases)try {test();++passed;std::cout<<"PASS "<<name<<'\n';}
        catch(const std::exception& e){++failed;std::cerr<<"FAIL "<<name<<": "<<e.what()<<'\n';}
    std::cout<<"RESULT "<<passed<<" passed, "<<failed<<" failed\n";return failed ? 1 : 0;
}

#include <Windows.h>
#include "Framegen/SourceDisplayClock.hpp"
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace cvr::framegen;
static unsigned checks;
static void check(bool b,const char* s){++checks;if(!b){std::fprintf(stderr,"FAIL %s\n",s);std::exit(1);}}
int main(){
    SourceDisplayClock clock;
    std::vector<std::pair<double,double>> samples;
    for(int frame=0;frame<240;++frame) {
        const auto time=1'000'000'000ll+frame*11'000'000ll;
        const double x=frame*.75+1./3.;
        const auto result=clock.ObserveAcceptedContinuous({true,time,{x,x+.2},2});
        check(result.addedSamples==2,"mixed/generated accepted pair contributes actual temporal positions");
        samples.emplace_back(x,double(time)*1e-9);samples.emplace_back(x+.2,double(time)*1e-9);
        if(samples.size()<100)continue;
        long double mx{},my{};const auto start=samples.size()-100;
        for(size_t i=start;i<samples.size();++i){mx+=samples[i].first;my+=samples[i].second;}mx/=100;my/=100;
        long double variance{},covariance{};
        for(size_t i=start;i<samples.size();++i){const long double dx=samples[i].first-mx;variance+=dx*dx;covariance+=dx*(samples[i].second-my);}
        const double expectedSlope=double(covariance/variance),expectedOffset=double(my-mx*expectedSlope);
        const auto fit=clock.Fit();
        check(fit.valid && std::abs(fit.secondsPerSourceId-expectedSlope)<1e-11 &&
            std::abs(fit.offsetSeconds-expectedOffset)<1e-9,"rolling fit agrees with independent long-double OLS");
    }
    const auto count=clock.Fit().samples;
    clock.ObserveAcceptedContinuous({true,4'000'000'000,{5,std::numeric_limits<double>::quiet_NaN()},2});
    clock.ObserveAcceptedContinuous({true,4'000'000'000,{-.5,4},2});
    clock.ObserveAcceptedContinuous({false,4'000'000'000,{6,7},2});
    check(clock.Fit().samples==count,"bad or unaccepted pair never partially modifies fit");
    clock.Reset();check(!clock.Fit().valid && clock.Fit().samples==0,"epoch reset clears native temporal fit");
    SourceDisplayClock raw,continuous;
    for(int i=0;i<60;++i){
        raw.ObserveAccepted({true,1'000'000'000+i*10'000'000,{2*i,2*i+1},2});
        continuous.ObserveAcceptedContinuous({true,1'000'000'000+i*10'000'000,{double(2*i),double(2*i+1)},2});
    }
    check(raw.Fit().secondsPerSourceId==continuous.Fit().secondsPerSourceId && raw.Fit().offsetSeconds==continuous.Fit().offsetSeconds,
          "integer AER contract remains bit-identical to integer-only use of extension");
    std::printf("PASS %u actual continuous source-clock checks\n",checks);
}

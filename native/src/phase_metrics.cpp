#include "cpvr/phase_metrics.hpp"
#include <algorithm>
namespace cpvr {
bool PhaseDurationWindow::observe(std::int64_t begin,std::int64_t end)noexcept{
    if(begin<0||end<begin){++rejected_;return false;}
    last_=end-begin;maximum_=std::max(maximum_,last_);++count_;
    values_[cursor_]=last_;cursor_=(cursor_+1)%capacity;size_=std::min(capacity,size_+1);return true;
}
PhaseDurationSummary PhaseDurationWindow::summary()const noexcept{
    PhaseDurationSummary out;out.count=count_;out.rejected=rejected_;out.last_ns=last_;out.maximum_ns=maximum_;out.window_samples=size_;
    if(!size_)return out;
    auto sorted=values_;std::sort(sorted.begin(),sorted.begin()+size_);
    std::int64_t quotient=0,remainder=0;
    for(unsigned i=0;i<size_;++i){quotient+=sorted[i]/size_;remainder+=sorted[i]%size_;}
    out.window_mean_ns=quotient+remainder/size_;
    out.p95_ns=sorted[(size_*95+99)/100-1];out.p99_ns=sorted[(size_*99+99)/100-1];return out;
}
}

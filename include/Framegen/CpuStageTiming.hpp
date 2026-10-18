#pragma once
#include <array>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#ifndef CVR_CPU_STAGE_TEST
#include <Windows.h>
#endif

extern void Log(const char*,...);
namespace cvr::framegen::cpu_timing {
enum class Stage : unsigned { PresentTotal, PresentOriginal, PresentFinishGpu, PresentPace,
    PresentCapture, PresentOverlay, PresentMirror, Ngx11Total, NgxObserveBegin, NgxObserveEnd,
    NgxPairedTotal, NgxHost, NgxCompanion, ReflexSleep, Count };
struct Statistic {uint64_t calls{},ticks{},peak{};};
using Snapshot=std::array<Statistic,unsigned(Stage::Count)>;
inline std::mutex statsMutex;
inline Snapshot stats{};
#ifdef CVR_CPU_STAGE_TEST
inline bool testEnabled=true;
inline uint64_t testClock{};
inline bool Enabled(){return testEnabled;}
inline uint64_t Now(){return testClock;}
inline uint64_t Frequency(){return 1000000;}
#else
inline bool Enabled(){static const bool enabled=[] {char value[8]{};return GetEnvironmentVariableA("CPVR_FG_CPU_TIMING",value,sizeof(value))==1&&value[0]=='1';}();return enabled;}
inline uint64_t Now(){LARGE_INTEGER value{};QueryPerformanceCounter(&value);return uint64_t(value.QuadPart);}
inline uint64_t Frequency(){static const uint64_t hz=[] {LARGE_INTEGER value{};QueryPerformanceFrequency(&value);return uint64_t(value.QuadPart);}();return hz;}
#endif
inline void Record(Stage stage,uint64_t ticks){std::lock_guard lock(statsMutex);auto& s=stats[unsigned(stage)];++s.calls;s.ticks+=ticks;s.peak=(std::max)(s.peak,ticks);}
inline Snapshot Take(){std::lock_guard lock(statsMutex);const auto result=stats;stats={};return result;}
class Scope {
public:
    explicit Scope(Stage stage,bool applicable=true):stage(stage),active(applicable&&Enabled()),start(active?Now():0){}
    ~Scope(){Finish();}
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
    void Finish(){if(!active)return;active=false;const auto end=Now();if(end>=start)Record(stage,end-start);}
private:
    Stage stage;bool active;uint64_t start;
};
// Inclusive CPU wall-time only. Nested stages must not be summed as GPU cost.
// Report once per 240 outer game Presents; no GPU query, fence or API mutation.
inline void PresentBoundary(){
    if(!Enabled())return;
    static std::atomic<uint64_t> presents{};const auto n=++presents;if(n%240)return;
    static const char* names[]{"PresentTotal","PresentOriginal","PresentFinishGpu","PresentPace","PresentCapture","PresentOverlay","PresentMirror",
        "Ngx11Total","NgxObserveBegin","NgxObserveEnd","NgxPairedTotal","NgxHost","NgxCompanion","ReflexSleep"};
    static_assert(std::size(names)==unsigned(Stage::Count));
    const auto values=Take();const auto hz=Frequency();if(!hz)return;
    for(unsigned i=0;i<values.size();++i){const auto& s=values[i];if(!s.calls)continue;
        Log("[FG-CPU-TIMING] present=%llu stage=%s calls=%llu totalMs=%.6f meanUs=%.3f maxUs=%.3f\n",n,names[i],s.calls,
            double(s.ticks)*1000/hz,double(s.ticks)*1e6/hz/s.calls,double(s.peak)*1e6/hz);
    }
}
}

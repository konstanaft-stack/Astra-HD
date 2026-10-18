#include "Framegen/Framegen.hpp"
#include "Framegen/SrHistoryIntegration.hpp"
#include "Framegen/NativeFgPairedSource.hpp"
#include "Framegen/NativeFgObservation.hpp"
#include "Framegen/GuideCapturePolicy.hpp"
#include "Framegen/GameGenerationOwner.hpp"
#include "Overlay/PpdPanel.hpp"
#include "Utils/DebugGate.hpp"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
extern "C" {
__declspec(dllexport) char CyberpunkVR_FramegenReport[2048]{};
__declspec(dllexport) std::atomic<uint32_t> CyberpunkVR_FramegenReportSeq{};
}

namespace cvr::framegen {
namespace {
std::mutex mutex;
std::mutex settingsWriteMutex;
Settings settings;
std::atomic<bool> enabled{false},metrics{false},captureProjection{false},aerHistory{false};
std::atomic<uint64_t> cascadeLendingEpoch{0};
std::atomic<bool> autoPace{true},pacingReady{false};
std::atomic<int64_t> displayPeriod{0};
Statistics stats;
PresentedCameraTracker presentedCamera;
PresentedGenerationTracker presentedGeneration;
PresentedGenerationTracker presentedAnyGeneration;
struct Sample {double stamp{},duration{};};
template<size_t N> struct Window {
    std::array<Sample,N> values{};
    size_t head{},count{};double started{};
    void Add(double now,double value=0) {if(!count)started=now;values[head]={now,value};head=(head+1)%N;count=std::min(count+1,N);}
    double Rate(double now) const {
        if(count<2)return 0;
        size_t used=0;double first=now,last=0;
        for(size_t i=0;i<count;++i) {
            first=std::min(first,values[i].stamp);last=std::max(last,values[i].stamp);
            if(values[i].stamp<=now && now-values[i].stamp<=2000)++used;
        }
        const double interval=(last-first)/(count-1);
        double window=std::min(2000.0,now-started+interval);
        // Menu rendering can fill the ring in less than two seconds. Preserve
        // that shorter measured window instead of capping its FPS at N/2.
        if(count==N)window=std::min(window,now-first+interval);
        // Use the full observation window for sparse events as well: two
        // repeats50ms apart in two seconds mean1 FPS, not20 FPS.
        return window>0 ? used*1000.0/window : 0;
    }
    double Peak(double now) const {
        double result=0;for(size_t i=0;i<count;++i)if(now-values[i].stamp<=2000)result=std::max(result,values[i].duration);
        return result;
    }
};
Window<512> real,generated,output,repeats,cpu,gpu;
FrameHistory<> cpuHistory,gpuHistory;
Statistics cached;
double nextRefresh{},nextSummary{},started{};
HistorySummary cpuSummary,gpuSummary;
uint64_t lastReal{};double lastRealTime{};
void ClearMetricsLocked() {
    presentedCamera={};
    presentedGeneration={};
    presentedAnyGeneration={};
    real={};generated={};output={};repeats={};cpu={};gpu={};cpuHistory.Clear();gpuHistory.Clear();
    cpuSummary={};gpuSummary={};nextRefresh=nextSummary=0;started=0;lastReal=0;lastRealTime=0;
    const bool active=stats.active;char status[sizeof(stats.status)]{};std::memcpy(status,stats.status,sizeof(status));
    stats={};stats.active=active;std::memcpy(stats.status,status,sizeof(status));cached={};
}
}
double NowMs() {
    static const double scale=[] {LARGE_INTEGER f{};QueryPerformanceFrequency(&f);return 1000.0/double(f.QuadPart);}();
    LARGE_INTEGER v{};QueryPerformanceCounter(&v);return double(v.QuadPart)*scale;
}
Settings GetSettings() {std::lock_guard lock(mutex);return settings;}
bool Enabled() {return enabled.load(std::memory_order_relaxed);}
bool CaptureProjectionEnabled() {return captureProjection.load(std::memory_order_relaxed);}
bool CaptureInputsEnabled() {
    const auto native=ReadNativeFgPolicyStatus();
    return NeedGuideCopies(CaptureProjectionEnabled(),aerHistory.load(std::memory_order_relaxed),
        Enabled(),NativeFgStereoRequested(),native.known,native.enabled);
}
uint64_t CascadeLendingEpoch() {return cascadeLendingEpoch.load(std::memory_order_acquire);}
bool MetricsEnabled() {return metrics.load(std::memory_order_relaxed);}
void SetSettings(Settings value) {
    std::lock_guard settingsLock(settingsWriteMutex);
    if(value.backend!=Backend::Nvidia && value.backend!=Backend::OwnNvidia)value.backend=Backend::FidelityFX;
    value.ownGeneratedFrames=std::clamp(value.ownGeneratedFrames,1,5);
    value.quality=std::clamp(value.quality,0,2);
    value.flowScale=value.flowScale==100 || value.flowScale==75 ? value.flowScale : 50;
    value.overlayCorner=std::clamp(value.overlayCorner,0,8);
    value.overlayLayout=std::clamp(value.overlayLayout,0,2);value.overlayFollow=value.overlayFollow!=0;
    value.historySeconds=value.historySeconds==10 || value.historySeconds==60 || value.historySeconds==120 ? value.historySeconds : 30;
    const auto bounded=[](float v,float lo,float hi,float fallback) {return std::isfinite(v) ? std::clamp(v,lo,hi) : fallback;};
    value.aerEmphasis=bounded(value.aerEmphasis,0,1,0);value.aerDeblur=bounded(value.aerDeblur,0,1,.5f);
    value.aerContrast=bounded(value.aerContrast,-1,1,0);value.aerVibrance=bounded(value.aerVibrance,-1,1,0);
    value.overlayCone=bounded(value.overlayCone,5,90,60);value.overlayScale=bounded(value.overlayScale,.5f,2,1);
    value.overlayDistance=bounded(value.overlayDistance,.5f,5,1.25f);
    value.overlayX=bounded(value.overlayX,-60,60,0);value.overlayY=bounded(value.overlayY,-45,45,0);
    bool metricsChanged=false,modeChanged=false;
    {std::lock_guard lock(mutex);
    metricsChanged=value.overlay!=settings.overlay;
    const bool disabledReportChanged=metricsChanged || value.enabled!=settings.enabled || value.backend!=settings.backend || !CyberpunkVR_FramegenReport[0];
    if(metricsChanged)ClearMetricsLocked();
    if(value.historySeconds!=settings.historySeconds)nextRefresh=nextSummary=0;
    if(value.aerV2!=settings.aerV2) {
        modeChanged=true;
        const auto epoch=cascadeLendingEpoch.load(std::memory_order_relaxed);
        // Exhaustion leaves native cascade processing enabled, never revives old snapshots.
        if(epoch!=UINT64_MAX)cascadeLendingEpoch.store(epoch+1,std::memory_order_release);
    }
    settings=value;
    enabled.store(value.enabled,std::memory_order_relaxed);
    // Every rendered color still needs its exact projection metadata. Extra
    // depth/MV copies serve AER history correction or an enabled generator.
    aerHistory.store(value.aerV2,std::memory_order_relaxed);
    captureProjection.store(true,std::memory_order_relaxed);
    metrics.store(value.overlay,std::memory_order_relaxed);
    autoPace.store(value.autoPace,std::memory_order_relaxed);
    if(!value.enabled) {pacingReady.store(false,std::memory_order_relaxed);stats.generationMs=0;}
    if(!value.overlay && disabledReportChanged) {
        CyberpunkVR_FramegenReportSeq.fetch_add(1,std::memory_order_acq_rel);
        std::snprintf(CyberpunkVR_FramegenReport,sizeof(CyberpunkVR_FramegenReport),
            "{\"enabled\":%d,\"backend\":%d,\"metrics\":0,\"status\":\"FPS telemetry disabled\"}",value.enabled,int(value.backend));
        CyberpunkVR_FramegenReportSeq.fetch_add(1,std::memory_order_release);
    }
    }
    if(modeChanged){cvr::ppd::InvalidatePpdSourceContext();ClearNativeFgPairedSource();}
    SetSrHistoryRequested(value.aerV2);
    if(value.overlay)WakeHardwareTelemetry();else if(metricsChanged)StopHardwareTelemetry();
}
RuntimeStatus GetRuntimeStatus() {
    std::lock_guard lock(mutex);RuntimeStatus result{};result.active=stats.active;
    std::memcpy(result.text,stats.status,sizeof(result.text));return result;
}
Statistics GetStatistics() {
    std::lock_guard lock(mutex);
    if(!settings.overlay)return stats;
    const auto now=NowMs();
    if(now<nextRefresh)return cached;
    nextRefresh=now+250;auto result=stats;
    result.cameraMode=presentedCamera.Read(now);
    result.nativePolicy=ReadNativeFgPolicyStatus();
    result.realFps=real.Rate(now);result.generatedFps=generated.Rate(now);
    const bool delivered=presentedAnyGeneration.RecentDelivery(result.cameraMode,now);
    result.active=settings.enabled && delivered;
    // A warning may leave SDK options unknown while valid G images still
    // reach XR. Report observed delivery separately from a confirmed policy.
    result.nativeGenerationActive=NativeFgPolicyObservationRequested() && presentedGeneration.RecentDelivery(result.cameraMode,now) &&
        (!result.nativePolicy.known || presentedGeneration.Active(result.nativePolicy,result.cameraMode,now));
    result.outputFps=output.Rate(now);result.repeatFps=repeats.Rate(now);
    result.cpuPeakMs=cpu.Peak(now);result.gpuPeakMs=gpu.Peak(now);
    if(now>=nextSummary) {cpuSummary=cpuHistory.Summarize(now,settings.historySeconds);gpuSummary=gpuHistory.Summarize(now,settings.historySeconds);nextSummary=now+1000;}
    result.historySamples=cpuSummary.samples;result.cpuAverageMs=cpuSummary.average;result.gpuAverageMs=gpuSummary.average;
    result.averageFps=cpuSummary.average>0 ? 1000/cpuSummary.average : 0;result.low1Fps=cpuSummary.low1;result.low01Fps=cpuSummary.low01;
    result.minimumFps=cpuSummary.peak>0 ? 1000/cpuSummary.peak : 0;
    result.maximumFps=cpuSummary.minimum>0 ? 1000/cpuSummary.minimum : 0;
    result.cpuP99Ms=cpuSummary.p99;result.cpuP999Ms=cpuSummary.p999;result.gpuP99Ms=gpuSummary.p99;result.gpuP999Ms=gpuSummary.p999;
    result.cpuGraph=cpuHistory.Graph(now,settings.historySeconds);result.gpuGraph=gpuHistory.Graph(now,settings.historySeconds);
    result.hardware=GetHardwareStatistics();result.elapsedSeconds=started ? (now-started)/1000 : 0;
    result.budgetMs=double(displayPeriod.load(std::memory_order_relaxed))*1e-6*(settings.enabled?2:1);
    if(result.budgetMs<=0)result.budgetMs=1000.0/90;
    const auto submissions=result.realSubmissions+result.generatedFrames+result.repeatedFrames;
    result.repeatPercent=submissions ? 100.0*result.repeatedFrames/submissions : 0;
    {
    // Read-only mirror of the visible statistics. Keep it available in the
    // ordinary launcher too; enabling broad diagnostics changes the workload.
    const bool native=NativeFgPolicyObservationRequested();
    const auto gameChoice=ReadGameGenerationChoice();
    const int reportEnabled=native ? (result.nativePolicy.known ? int(result.nativePolicy.enabled) :
        gameChoice.known&&gameChoice.provider==GameGenerationProvider::Off&&!NativeFgGameWasEnabled()&&!result.nativeGenerationActive ? 0 : -1) : int(settings.enabled);
    const bool reportActive=native ? result.nativeGenerationActive : result.active;
    const unsigned multiplier=native ? (result.nativePolicy.known ? result.nativePolicy.multiplier :
        gameChoice.known&&gameChoice.provider==GameGenerationProvider::Nvidia ? gameChoice.multiplier : 0u) :
        settings.enabled ? (settings.backend==Backend::OwnNvidia ? unsigned(settings.ownGeneratedFrames+1) : 2u) : 0u;
    char nativeStatus[80]{};
    if(native) {
        if(!result.nativePolicy.known)std::snprintf(nativeStatus,sizeof(nativeStatus),reportActive?"Native NVIDIA accepted G; policy unknown":"Native NVIDIA policy unknown; no recent G");
        else if(!result.nativePolicy.enabled)std::snprintf(nativeStatus,sizeof(nativeStatus),"Native NVIDIA off");
        else std::snprintf(nativeStatus,sizeof(nativeStatus),"Native NVIDIA X%u %s",multiplier,reportActive?"accepted G":"waiting for G");
    }
    CyberpunkVR_FramegenReportSeq.fetch_add(1,std::memory_order_acq_rel);
    std::snprintf(CyberpunkVR_FramegenReport,sizeof(CyberpunkVR_FramegenReport),
        "{\"enabled\":%d,\"active\":%d,\"backend\":%d,\"metrics\":1,\"real_fps\":%.3f,\"fg_fps\":%.3f,\"output_fps\":%.3f,\"repeats_fps\":%.3f,"
        "\"cpu_ms\":%.3f,\"gpu_ms\":%.3f,\"generation_ms\":%.3f,\"real\":%llu,\"generated\":%llu,\"real_submitted\":%llu,\"repeated\":%llu,\"skipped\":%llu,\"vram_bytes\":%llu,"
        "\"inputs\":[%llu,%llu],\"average_fps\":%.3f,\"minimum_fps\":%.3f,\"maximum_fps\":%.3f,\"low1\":%.3f,\"low01\":%.3f,\"cpu_usage\":%.2f,\"gpu_usage\":%.2f,\"gpu_temp\":%.1f,"
        "\"gpu_memory_used\":%llu,\"gpu_memory_total\":%llu,\"ram_used\":%llu,\"ram_total\":%llu,\"status\":\"%s\","
        "\"native_policy_known\":%d,\"requested_multiplier\":%u,\"presented_camera\":%u,\"hardware_sequence\":%llu,\"hardware_age_ms\":%.1f,"
        "\"game_fg_selection_fresh\":%d,\"game_fg_provider\":%u,\"game_fg_selected_multiplier\":%u}",reportEnabled,reportActive,native?3:int(settings.backend),result.realFps,result.generatedFps,
        result.outputFps,result.repeatFps,result.cpuMs,result.gpuMs,result.generationMs,result.realFrames,result.generatedFrames,result.realSubmissions,result.repeatedFrames,result.skipped,
        result.vramBytes,result.inputFrames[0],result.inputFrames[1],result.averageFps,result.minimumFps,result.maximumFps,result.low1Fps,result.low01Fps,
        result.hardware.cpuUsage,result.hardware.gpuUsage,result.hardware.gpuTemperature,result.hardware.gpuMemoryUsed,result.hardware.gpuMemoryTotal,
        result.hardware.ramUsed,result.hardware.ramTotal,native?nativeStatus:result.status,result.nativePolicy.known,multiplier,unsigned(result.cameraMode),
        result.hardware.sequence,result.hardware.sampledAtMs>0?now-result.hardware.sampledAtMs:-1,
        int(gameChoice.known),unsigned(gameChoice.provider),gameChoice.multiplier);
    CyberpunkVR_FramegenReportSeq.fetch_add(1,std::memory_order_release);
    }
    cached=result;return result;
}
void ResetStatistics() {
    std::lock_guard lock(mutex);ClearMetricsLocked();
}
void Status(const char* text,bool active) {
    std::lock_guard lock(mutex);stats.active=active;std::snprintf(stats.status,sizeof(stats.status),"%s",text);
}
void OnRealFrame(uint64_t serial,double now) {
    if(!MetricsEnabled())return;std::lock_guard lock(mutex);if(!settings.overlay || !serial || serial==lastReal)return;
    if(!started)started=now;
    if(lastRealTime>0 && now>lastRealTime) {stats.cpuMs=now-lastRealTime;cpu.Add(now,stats.cpuMs);cpuHistory.Add(now,stats.cpuMs);}
    lastReal=serial;lastRealTime=now;real.Add(now);++stats.realFrames;
}
void OnSubmitted(uint64_t,bool synth,bool repeat,double now) {
    if(!MetricsEnabled())return;std::lock_guard lock(mutex);if(!settings.overlay)return;
    if(repeat) {repeats.Add(now);++stats.repeatedFrames;return;}
    output.Add(now);if(synth) {generated.Add(now);++stats.generatedFrames;presentedAnyGeneration.Generated(presentedCamera.Read(now),0,now);}else ++stats.realSubmissions;
}
void OnCameraSubmitted(PresentedCamera mode,double now) {
    if(!MetricsEnabled())return;
    std::lock_guard lock(mutex);if(settings.overlay){presentedCamera.Accepted(mode,now);presentedGeneration.Camera(mode);presentedAnyGeneration.Camera(mode);}
}
void OnNativeGeneratedSubmitted(unsigned revision,PresentedCamera mode,double now) {
    if(!MetricsEnabled())return;
    std::lock_guard lock(mutex);if(settings.overlay)presentedGeneration.Generated(mode,revision,now);
}
void OnGpuFrame(double ms) {
    if(!MetricsEnabled() || !std::isfinite(ms) || ms<0 || ms>10000)return;
    std::lock_guard lock(mutex);if(!settings.overlay)return;stats.gpuValid=true;stats.gpuMs=ms;const auto now=NowMs();gpu.Add(now,ms);gpuHistory.Add(now,ms);
}
void OnGeneration(double ms) {if(!MetricsEnabled())return;std::lock_guard lock(mutex);if(settings.overlay)stats.generationMs=ms;}
void OnInput(unsigned eye,uint32_t w,uint32_t h) {
    if(eye>1 || !MetricsEnabled())return;std::lock_guard lock(mutex);if(!settings.overlay)return;++stats.inputFrames[eye];
    stats.renderWidth[eye]=w;stats.renderHeight[eye]=h;
    stats.inputsReady=stats.inputFrames[0] && stats.inputFrames[1];
}
void OnSkip() {if(!MetricsEnabled())return;std::lock_guard lock(mutex);if(settings.overlay)++stats.skipped;}
void SetVram(uint64_t bytes) {if(!MetricsEnabled())return;std::lock_guard lock(mutex);if(settings.overlay)stats.vramBytes=bytes;}
void DisplayPeriod(int64_t ns) {if(ns>=4000000 && ns<=40000000)displayPeriod.store(ns,std::memory_order_relaxed);}
void PacingReady(bool ready) {pacingReady.store(ready,std::memory_order_relaxed);}
void PaceRenderedFrame() {
    struct Timer {HANDLE handle{};double last{};~Timer(){if(handle)CloseHandle(handle);}};
    // The game's Present jobs can run on different workers. A TLS deadline
    // limits each worker independently, allowing the combined stream to run
    // faster than half refresh. Serialize one schedule for that stream.
    static Timer timer;
    static std::mutex paceMutex;
    std::lock_guard paceLock(paceMutex);
    const auto period=displayPeriod.load(std::memory_order_relaxed);
    if(!Enabled() || !autoPace.load(std::memory_order_relaxed) || !pacingReady.load(std::memory_order_relaxed) || !period) {timer.last=0;return;}
    const double now=NowMs(),interval=double(period)*2e-6;
    const double remaining=timer.last+interval-now;
    if(remaining>.2 && remaining<=80) {
        if(!timer.handle)timer.handle=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
        if(timer.handle) {
            LARGE_INTEGER due{};due.QuadPart=-static_cast<LONGLONG>(remaining*10000.0);
            if(SetWaitableTimerEx(timer.handle,&due,0,nullptr,nullptr,nullptr,0))WaitForSingleObject(timer.handle,100);
        }
    }
    const auto done=NowMs();timer.last=timer.last && done<timer.last+interval*1.5 ? timer.last+interval : done;
}
bool ParseSetting(const char* line,Settings& value) {
    int n{};float f{};
    if(sscanf_s(line,"xr_framegen = %d",&n)==1)value.enabled=n!=0;
    else if(sscanf_s(line,"xr_framegen_overlay = %d",&n)==1)value.overlay=n!=0;
    else if(sscanf_s(line,"xr_framegen_pace = %d",&n)==1)value.autoPace=n!=0;
    else if(sscanf_s(line,"xr_framegen_backend = %d",&n)==1)value.backend=n==2?Backend::OwnNvidia:(n==1?Backend::Nvidia:Backend::FidelityFX);
    else if(sscanf_s(line,"xr_aer_v2 = %d",&n)==1)value.aerV2=n!=0;
    else if(sscanf_s(line,"xr_native_generated_frames = %d",&n)==1)value.ownGeneratedFrames=n;
    else if(sscanf_s(line,"xr_aer_emphasis = %f",&f)==1)value.aerEmphasis=f;
    else if(sscanf_s(line,"xr_aer_deblur = %f",&f)==1)value.aerDeblur=f;
    else if(sscanf_s(line,"xr_aer_contrast = %f",&f)==1)value.aerContrast=f;
    else if(sscanf_s(line,"xr_aer_vibrance = %f",&f)==1)value.aerVibrance=f;
    else if(sscanf_s(line,"xr_framegen_quality = %d",&n)==1)value.quality=n;
    else if(sscanf_s(line,"xr_framegen_flow_scale = %d",&n)==1)value.flowScale=n;
    else if(sscanf_s(line,"xr_framegen_overlay_corner = %d",&n)==1)value.overlayCorner=n;
    else if(sscanf_s(line,"xr_framegen_overlay_layout = %d",&n)==1)value.overlayLayout=n;
    else if(sscanf_s(line,"xr_framegen_overlay_follow = %d",&n)==1)value.overlayFollow=n;
    else if(sscanf_s(line,"xr_framegen_history = %d",&n)==1)value.historySeconds=n;
    else if(sscanf_s(line,"xr_framegen_overlay_cone = %f",&f)==1)value.overlayCone=f;
    else if(sscanf_s(line,"xr_framegen_overlay_scale = %f",&f)==1)value.overlayScale=f;
    else if(sscanf_s(line,"xr_framegen_overlay_distance = %f",&f)==1)value.overlayDistance=f;
    else if(sscanf_s(line,"xr_framegen_overlay_x = %f",&f)==1)value.overlayX=f;
    else if(sscanf_s(line,"xr_framegen_overlay_y = %f",&f)==1)value.overlayY=f;
    else return false;
    return true;
}
void WriteSettings(void* file,const Settings& s) {
    std::fprintf(static_cast<FILE*>(file),"xr_aer_v2=%d\nxr_native_generated_frames=%d\nxr_aer_emphasis=%.4f\nxr_aer_deblur=%.4f\nxr_aer_contrast=%.4f\nxr_aer_vibrance=%.4f\n",
        s.aerV2,s.ownGeneratedFrames,s.aerEmphasis,s.aerDeblur,s.aerContrast,s.aerVibrance);
    std::fprintf(static_cast<FILE*>(file),"xr_framegen=%d\nxr_framegen_overlay=%d\nxr_framegen_pace=%d\nxr_framegen_backend=%d\nxr_framegen_quality=%d\nxr_framegen_flow_scale=%d\nxr_framegen_overlay_corner=%d\n",
        s.enabled,s.overlay,s.autoPace,int(s.backend),s.quality,s.flowScale,s.overlayCorner);
    std::fprintf(static_cast<FILE*>(file),"xr_framegen_overlay_layout=%d\nxr_framegen_overlay_follow=%d\nxr_framegen_history=%d\nxr_framegen_overlay_cone=%.2f\nxr_framegen_overlay_scale=%.3f\nxr_framegen_overlay_distance=%.3f\nxr_framegen_overlay_x=%.2f\nxr_framegen_overlay_y=%.2f\n",
        s.overlayLayout,s.overlayFollow,s.historySeconds,s.overlayCone,s.overlayScale,s.overlayDistance,s.overlayX,s.overlayY);
}
}

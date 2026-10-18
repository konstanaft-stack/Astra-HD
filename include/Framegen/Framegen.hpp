#pragma once
#include <atomic>
#include <cstdint>
#include "Framegen/HardwareTelemetry.hpp"
#include "Framegen/History.hpp"
#include "Framegen/PresentationStatus.hpp"

namespace cvr::framegen {
enum class Backend:int { FidelityFX=0,Nvidia=1,OwnNvidia=2 };
struct Settings {
    bool enabled=false,overlay=false,autoPace=true;
    Backend backend=Backend::Nvidia;
    // Stereo source cadence is independent of optional frame generation.
    bool aerV2=false;
    int ownGeneratedFrames=1;
    float aerEmphasis=0,aerDeblur=.5f,aerContrast=0,aerVibrance=0;
    int quality=2,flowScale=50,overlayCorner=1;
    int overlayLayout=0,overlayFollow=1,historySeconds=30;
    float overlayCone=60,overlayScale=1,overlayDistance=1.25f,overlayX=0,overlayY=0;
};
struct Statistics {
    PresentedCamera cameraMode{};
    NativePolicyStatus nativePolicy{};
    bool nativeGenerationActive{};
    double realFps{},generatedFps{},outputFps{},repeatFps{};
    double cpuMs{},cpuPeakMs{},gpuMs{},gpuPeakMs{},generationMs{};
    double averageFps{},minimumFps{},maximumFps{},low1Fps{},low01Fps{},cpuAverageMs{},gpuAverageMs{},cpuP99Ms{},cpuP999Ms{},gpuP99Ms{},gpuP999Ms{};
    double elapsedSeconds{},budgetMs{},repeatPercent{};
    uint32_t historySamples{};
    std::array<float,GraphPoints> cpuGraph{},gpuGraph{};
    HardwareStatistics hardware;
    uint64_t realFrames{},generatedFrames{},repeatedFrames{},skipped{},vramBytes{};
    uint64_t realSubmissions{};
    uint64_t inputFrames[2]{};
    uint32_t renderWidth[2]{},renderHeight[2]{};
    bool active{},gpuValid{},inputsReady{};
    char status[160]="Выключено";
};
Settings GetSettings();
bool Enabled();
bool CaptureInputsEnabled();
bool CaptureProjectionEnabled();
// Odd epochs disable VRCAM cascade borrowing (AER owns the MAIN source).
// Every mode transition also invalidates snapshots retained by the other mode.
uint64_t CascadeLendingEpoch();
bool MetricsEnabled();
void SetSettings(Settings value);
Statistics GetStatistics();
struct RuntimeStatus {bool active{};char text[160]{};};
RuntimeStatus GetRuntimeStatus();
void ResetStatistics();
void Status(const char* text,bool active=false);
void OnRealFrame(uint64_t serial,double nowMs);
void OnSubmitted(uint64_t serial,bool generated,bool repeat,double nowMs);
void OnCameraSubmitted(PresentedCamera mode,double nowMs);
void OnNativeGeneratedSubmitted(unsigned policyRevision,PresentedCamera mode,double nowMs);
void OnGpuFrame(double milliseconds);
void OnGeneration(double milliseconds);
void OnInput(unsigned eye,uint32_t width,uint32_t height);
void OnSkip();
void SetVram(uint64_t bytes);
double NowMs();
void DisplayPeriod(int64_t nanoseconds);
void PacingReady(bool);
void PaceRenderedFrame();
bool ParseSetting(const char*,Settings&);
void WriteSettings(void* file,const Settings&);
}

#pragma once
#include <array>
#include <cstdint>
struct ID3D12Device;
namespace cvr::framegen {
struct HardwareStatistics {
    uint64_t sequence{};
    double cpuUsage=-1,gpuUsage=-1,gpuTemperature=-1,coreMaximum=-1,sampleMs{},sampledAtMs{};
    uint64_t ramUsed{},ramTotal{},appWorkingSet{},appCommit{};
    uint64_t gpuMemoryUsed{},gpuMemoryTotal{},gpuProcessUsed{},gpuProcessBudget{};
    bool gpuMemoryValid{},gpuProcessMemoryValid{},fresh{};
    uint32_t coreCount{};
    std::array<float,128> cores{};
    char gpuName[96]{},cpuName[96]{};
};
inline void ValidateHardwareFreshness(HardwareStatistics& result,double now) {
    const auto age=now-result.sampledAtMs;
    result.fresh=result.sequence && result.sampledAtMs>0 && age>=0 && age<=3000;
    if(!result.fresh) {
        result.cpuUsage=result.gpuUsage=result.gpuTemperature=result.coreMaximum=-1;
        result.gpuMemoryValid=result.gpuProcessMemoryValid=false;
        result.ramTotal=0;
    }
}
void ConfigureHardwareTelemetry(ID3D12Device*);
void WakeHardwareTelemetry();
void StopHardwareTelemetry();
HardwareStatistics GetHardwareStatistics();
}

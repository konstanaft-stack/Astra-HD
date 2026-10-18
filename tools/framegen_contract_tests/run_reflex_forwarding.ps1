param([Parameter(Mandatory)][string]$AuditDirectory)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$a=[IO.Path]::GetFullPath($AuditDirectory);[IO.Directory]::CreateDirectory($a)|Out-Null
$text=[IO.File]::ReadAllText((Join-Path $repo 'src/Framegen/NativeFgPolicyObservation.cpp'))
$s=$text.IndexOf('sl::Result ObserveReflexSleep(');$e=$text.IndexOf('void EnsureCpuReflexTiming()', $s)
if($s -lt 0 -or $e -lt 0){throw 'Callback boundaries changed'}
$prefix=@'
#define CVR_CPU_STAGE_TEST
#include "Framegen/CpuStageTiming.hpp"
#include "Framegen/NativeReflexTypes.hpp"
#include <cstdio>
#include <cstdlib>
namespace cpu_timing=cvr::framegen::cpu_timing;
void Log(const char*,...){}
sl::PFun_slReflexSleep* originalReflexSleep{};
sl::PFun_slReflexSetOptions* originalReflexOptions{};
sl::PFun_slReflexGetState* originalReflexState{};
'@
$suffix=@'
void Check(bool ok,const char* why){if(!ok){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}}
unsigned sleeps{},sets{},states{};const sl::FrameToken* lastToken{};const sl::ReflexOptions* lastOptions{};sl::ReflexState* lastState{};
sl::Result Sleep(const sl::FrameToken& token){++sleeps;lastToken=&token;cpu_timing::testClock+=123;return sl::Result::eOk;}
sl::Result Set(const sl::ReflexOptions& options){++sets;lastOptions=&options;return sl::Result::eErrorInvalidParameter;}
sl::Result State(sl::ReflexState& state){++states;lastState=&state;state.latencyReportAvailable=true;state.frameReport[0].frameID=123;state.frameReport[0].gpuFrameTimeUs=400;state.frameReport[0].gpuActiveRenderTimeUs=100;return sl::Result::eOk;}
struct Token:sl::FrameToken {operator uint32_t() const override{return 77;}};
int main(){Token token;sl::ReflexOptions options;options.mode=sl::ReflexMode::eLowLatency;options.frameLimitUs=34567;
    originalReflexSleep=&Sleep;originalReflexOptions=&Set;originalReflexState=&State;
    Check(ObserveReflexSleep(token)==sl::Result::eOk&&sleeps==1&&lastToken==&token,"original sleep identity/result/one call");
    Check(ObserveReflexOptions(options)==sl::Result::eErrorInvalidParameter&&sets==1&&lastOptions==&options,"original options object and failure unchanged");
    Check(options.frameLimitUs==34567&&options.mode==sl::ReflexMode::eLowLatency,"diagnostic cannot disable pacing");
    Check(states==0,"no extra GetState to produce diagnostics");sl::ReflexState state;
    Check(ObserveReflexState(state)==sl::Result::eOk&&states==1&&lastState==&state&&state.frameReport[0].frameID==123&&state.frameReport[0].gpuActiveRenderTimeUs==100,"existing GetState forwarded once, returned report preserved");
    const auto stats=cpu_timing::Take()[unsigned(cpu_timing::Stage::ReflexSleep)];Check(stats.calls==1&&stats.ticks==123,"records original wait wall time");
    std::puts("PASS: actual Reflex observer callbacks forward originals exactly once without parameter changes");
}
'@
[IO.File]::WriteAllText("$a/probe.cpp",$prefix+"`n"+$text.Substring($s,$e-$s)+"`n"+$suffix,[Text.UTF8Encoding]::new($false))
$c='<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc/VC/Tools/MSVC/14.44.35207'
$sdk='<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc/Windows Kits/10'
$env:INCLUDE="$c/include;$sdk/Include/10.0.26100.0/ucrt"
$env:LIB="$c/lib/x64;$sdk/Lib/10.0.26100.0/ucrt/x64;$sdk/Lib/10.0.26100.0/um/x64"
& "$c/bin/Hostx64/x64/cl.exe" /nologo /EHsc /std:c++20 /W4 "/I$repo/include" "/I$repo/externals/streamline/include" "$a/probe.cpp" "/Fo$a/probe.obj" "/Fe$a/probe.exe" 2>&1|Set-Content "$a/build.log"
if($LASTEXITCODE){Get-Content "$a/build.log" -Tail 12;throw 'Compile failed'}
& "$a/probe.exe" 2>&1|Tee-Object -FilePath "$a/result.log"
if($LASTEXITCODE){exit $LASTEXITCODE}

param([Parameter(Mandatory)][string]$AuditDirectory)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$a=[IO.Path]::GetFullPath($AuditDirectory);[IO.Directory]::CreateDirectory($a)|Out-Null
$source=Join-Path $repo 'include/Runtimes/OpenXRManager.hpp'
$text=[IO.File]::ReadAllText($source)
$s=$text.IndexOf('    bool UseThreadedSubmit() const {');$e=$text.IndexOf('    int GetCurrentRenderEyeIndex()', $s)
if($s -lt 0 -or $e -lt 0){throw 'Production boundary changed'}
$prefix=@'
#include <atomic>
#include <cstdio>
#include <cstdlib>
namespace cvr::framegen {
bool projection{},guides{};
bool CaptureProjectionEnabled(){return projection;}
bool CaptureInputsEnabled(){return guides;}
}
int CyberpunkVR_ThreadedMonoSubmit=-1;
struct Manager {
std::atomic<bool> m_monoSubmitEnabled{true},m_runtimeIsVirtualDesktop{true};
'@
$suffix=@'
};
void Check(bool ok,const char* why){if(!ok){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}}
int main(){Manager m;
    cvr::framegen::projection=true;cvr::framegen::guides=false;
    Check(m.UseThreadedSubmit(),"projection-only must retain display-rate thread on Virtual Desktop");
    CyberpunkVR_ThreadedMonoSubmit=0;
    Check(m.UseThreadedSubmit(),"guide allocation must not change existing producer ownership override");
    m.m_monoSubmitEnabled=false;Check(!m.UseThreadedSubmit(),"disabled submission has no thread owner");
    m.m_monoSubmitEnabled=true;cvr::framegen::projection=false;
    Check(!m.UseThreadedSubmit(),"legacy forced inline preserved without projection producer");
    CyberpunkVR_ThreadedMonoSubmit=1;Check(m.UseThreadedSubmit(),"legacy forced threaded preserved");
    CyberpunkVR_ThreadedMonoSubmit=-1;Check(!m.UseThreadedSubmit(),"legacy VD auto preserved without projection capture");
    m.m_runtimeIsVirtualDesktop=false;Check(m.UseThreadedSubmit(),"legacy other-runtime auto preserved");
    std::puts("PASS: production submission ownership independent of optional guide copies");
}
'@
[IO.File]::WriteAllText("$a/probe.cpp",$prefix+"`n"+$text.Substring($s,$e-$s)+"`n"+$suffix,[Text.UTF8Encoding]::new($false))
$c='<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc/VC/Tools/MSVC/14.44.35207'
$sdk='<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc/Windows Kits/10'
$env:INCLUDE="$c/include;$sdk/Include/10.0.26100.0/ucrt"
$env:LIB="$c/lib/x64;$sdk/Lib/10.0.26100.0/ucrt/x64;$sdk/Lib/10.0.26100.0/um/x64"
& "$c/bin/Hostx64/x64/cl.exe" /nologo /EHsc /std:c++20 /W4 "$a/probe.cpp" "/Fo$a/probe.obj" "/Fe$a/probe.exe" 2>&1|Set-Content "$a/build.log"
if($LASTEXITCODE){throw 'Compile failed'}
& "$a/probe.exe" 2>&1|Tee-Object -FilePath "$a/result.log"
if($LASTEXITCODE){exit $LASTEXITCODE}

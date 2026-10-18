param([string]$Stage='green',[switch]$Replay)
$ErrorActionPreference='Stop'
$projectTask='<WORKSPACE_ROOT>/research/2026-09-30-cpvr-open-nvidia';$sourceTask=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if($Stage -notmatch '^[A-Za-z0-9_-]+$'){throw 'Invalid stage name'}
if(@(Get-Process -Name Cyberpunk2077,NMS -ErrorAction SilentlyContinue).Count){throw 'Game active; defer CPU build'}
$mutexTask=[Threading.Mutex]::new($false,'Local\IRD.SingleBuildTestDeploy.v1')
if(-not $mutexTask.WaitOne(0)){$mutexTask.Dispose();throw 'Build/test/deploy mutex held'}
$outputTask=Join-Path "$projectTask/build/tofu-main-capture-catalogue" $Stage;New-Item -ItemType Directory -Path $outputTask -Force|Out-Null
Push-Location $outputTask
try {
    $toolTask='<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc';$vcTask="$toolTask/VC/Tools/MSVC/14.44.35207";$sdkTask="$toolTask/Windows Kits/10";$verTask='10.0.26100.0'
    $env:INCLUDE="$vcTask/include;"+((@('ucrt','shared','um','winrt','cppwinrt')|ForEach-Object{"$sdkTask/Include/$verTask/$_"})-join ';')
    $env:LIB="$vcTask/lib/x64;$sdkTask/Lib/$verTask/ucrt/x64;$sdkTask/Lib/$verTask/um/x64"
    $env:PATH="$vcTask/bin/Hostx64/x64;$sdkTask/bin/$verTask/x64;"+$env:PATH
    if($Replay){
        & "$vcTask/bin/Hostx64/x64/cl.exe" /nologo /EHsc /std:c++20 /utf-8 /MT /DNOMINMAX /DWIN32_LEAN_AND_MEAN "/I$sourceTask/include" "$sourceTask/tools/framegen_contract_tests/main_capture_ring_replay.cpp" /Fe:main_capture_ring_replay.exe *> 'build.log'
        if($LASTEXITCODE -ne 0){Get-Content 'build.log' -Tail 25;throw 'Ring replay compile failed'}
        & './main_capture_ring_replay.exe' *> 'run.log';$resultTask=$LASTEXITCODE;Get-Content 'run.log'
        if($resultTask -ne 0){throw "Ring replay exit $resultTask"};return
    }
    if(Test-Path "$sourceTask/include/Runtimes/MainCaptureCatalogue.hpp") {
        $sourceTextTask=[IO.File]::ReadAllText("$sourceTask/src/Runtimes/OpenXRCapture.cpp")
        $startTask=$sourceTextTask.IndexOf('std::vector<cvr::framegen::Frame> OpenXRManager::CollectReadyMainCaptures(')
        if($startTask -lt 0){throw 'Production collector absent'}
        $openedTask=$sourceTextTask.IndexOf('{',$startTask);$depthTask=1;$endTask=$openedTask+1
        while($depthTask){if($sourceTextTask[$endTask] -eq '{'){$depthTask++};if($sourceTextTask[$endTask] -eq '}'){$depthTask--};$endTask++}
        [IO.File]::WriteAllText((Join-Path $outputTask 'collect_main_captures.inc'),$sourceTextTask.Substring($startTask,$endTask-$startTask))
        $pubStartTask=$sourceTextTask.IndexOf('            // Catalogue entries retain CPU camera/identity metadata')
        $pubEndTask=$sourceTextTask.IndexOf('            SetD3DNamef(m_monoCapturedFrame.texture',$pubStartTask)
        if($pubStartTask -lt 0 -or $pubEndTask -lt 0){throw 'Production publication fragment absent'}
        [IO.File]::WriteAllText((Join-Path $outputTask 'publish_main_capture.inc'),$sourceTextTask.Substring($pubStartTask,$pubEndTask-$pubStartTask))
    }
    & "$vcTask/bin/Hostx64/x64/cl.exe" /nologo /EHsc /std:c++20 /utf-8 /MT /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DXR_USE_PLATFORM_WIN32 /DXR_USE_GRAPHICS_API_D3D12 "/I$sourceTask/include" "/I$projectTask/native/include" "/I$projectTask/build/tofu-ru/_deps/openxr-src/include" "/I$outputTask" "$sourceTask/tools/framegen_contract_tests/main_capture_catalogue.cpp" /Fe:main_capture_catalogue.exe *> 'build.log'
    if($LASTEXITCODE -ne 0){Get-Content 'build.log' -Tail 30;throw 'CPU fixture compile failed'}
    & './main_capture_catalogue.exe' *> 'run.log';$resultTask=$LASTEXITCODE;Get-Content 'run.log'
    if($resultTask -ne 0){throw "CPU fixture exit $resultTask"}
} finally {Pop-Location;$mutexTask.ReleaseMutex();$mutexTask.Dispose()}

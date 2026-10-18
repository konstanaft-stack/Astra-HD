param([string]$Version=(Get-Date -Format 'yyyyMMdd-HHmmss'),[string]$Dotnet=(Join-Path $PSScriptRoot 'toolchain/dotnet/dotnet.exe'),[string]$UsvfsRoot=(Join-Path $PSScriptRoot 'components/usvfs'))
$ErrorActionPreference='Stop'
if($Version -notmatch '^[a-zA-Z0-9._-]+$'){throw 'Invalid release name'}
$out=Join-Path $PSScriptRoot "releases/$Version"
if(Test-Path $out){throw 'Release directory already exists; choose a new name to avoid overwriting a running app.'}
$env:DOTNET_CLI_TELEMETRY_OPTOUT='1'
$env:DOTNET_GENERATE_ASPNET_CERTIFICATE='false'
foreach($name in @('usvfs_x64.dll','usvfs_x86.dll','usvfs_proxy_x64.exe','usvfs_proxy_x86.exe')){if(-not(Test-Path -LiteralPath (Join-Path $UsvfsRoot $name) -PathType Leaf)){throw ('Missing usvfs component: '+$name)}}
& $Dotnet publish "$PSScriptRoot/VRPorts.csproj" -c Release -r win-x64 --self-contained true -o $out --nologo -v minimal
if($LASTEXITCODE -ne 0){throw 'Build failed'}
& $Dotnet publish "$PSScriptRoot/helpers/VfsHost/VfsHost.csproj" -c Release -r win-x64 --self-contained true -o (Join-Path $out 'helpers/vfs') --nologo -v minimal
if($LASTEXITCODE -ne 0){throw 'VFS host build failed'}
[IO.Directory]::CreateDirectory((Join-Path $out 'components/usvfs'))|Out-Null
foreach($name in @('usvfs_x64.dll','usvfs_x86.dll','usvfs_proxy_x64.exe','usvfs_proxy_x86.exe')){Copy-Item -LiteralPath (Join-Path $UsvfsRoot $name) -Destination (Join-Path $out ('components/usvfs/'+$name))}
foreach($script in Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.ps1' -File){Copy-Item -LiteralPath $script.FullName -Destination (Join-Path $out $script.Name)}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'licenses') -Destination (Join-Path $out 'licenses') -Recurse
if(Test-Path -LiteralPath (Join-Path $PSScriptRoot 'integrations/vortex')){[IO.Directory]::CreateDirectory((Join-Path $out 'integrations/vortex'))|Out-Null;Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'integrations/vortex/read-catalog.js') -Destination (Join-Path $out 'integrations/vortex/read-catalog.js')}
$manifest=@(Get-ChildItem -LiteralPath $out -Recurse -File | ForEach-Object {@{path=[IO.Path]::GetRelativePath($out,$_.FullName);sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}})
$manifest|ConvertTo-Json -Depth 3|Set-Content -LiteralPath (Join-Path $out 'release-files.json') -Encoding utf8
Write-Output "Built: $out/VRPorts.exe. Shortcut is not changed automatically."

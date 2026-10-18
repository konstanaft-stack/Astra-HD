# External dependencies and build boundary

This is a portable SOURCE backup of the current VR-CENTR316 code with an independently authored public theme. It is NOT a game installer, a compiled VR-CENTR release, a complete Astra HD source backup or a ready VR bundle. No game/driver/NVIDIA/Luke/Opti/MFG binaries, saves, personal config or prepared cleanbaseline are included. No download URL is claimed.

Required to build: Windows x64, .NET8 SDK; Avalonia12.1.0 packages restore through NuGet. PowerShell7 (pwsh) is required by backend/launcher. Current Build.ps1 also requires separately obtained usvfs_x64.dll/usvfs_x86.dll/usvfs_proxy_x64.exe/usvfs_proxy_x86.exe; provide their folder through -UsvfsRoot and include applicable GPL/source notices when distributing binaries. Build uses a self-contained win-x64 application plus helpers/VfsHost. Supply installed dotnet through -Dotnet; the private toolchain is deliberately not included.

```powershell
$dotnet = (Get-Command dotnet -ErrorAction Stop).Source
$usvfs = Read-Host 'Folder containing the four separately obtained usvfs components'
.\Build.ps1 -Version public-source-review -Dotnet $dotnet -UsvfsRoot $usvfs
```

Commands are instructions, not a completed build. Root must compile/test this public theme and compare backend behaviour before a release. Existing tests include project-specific fixtures and some absolute path assumptions; no full suite portability pass is claimed.

To use a future compiled application: copy config.example.json to a private config.json and set all paths to your machine or use the included first-start wrapper alongside the compiledEXE. A separately prepared, licensed CyberpunkSteam installation/VRbundle/baseline is still required. Source ZIP alone cannot create these. Never copy a developer-machine config onto another PC as if its absolute paths were portable.

Provenance: frozen316 source737e89b with actual SettingsProfilesCF56 patch; publicApp/MainWindow theme; privateStyles/Compact and two photographs excluded. Publicwindow keeps current1080x720. Currentcsproj/Build/VfsHost and NR/sharedUI hooks preserved. MIT applies only to contributor-ownedmaterial; retain LICENSE, THIRD-PARTY and licenses/ dependency notices. OldNODIXnotices documenthistory, not redistributionpermission for excludedprivateart.

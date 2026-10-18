$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$configPath = Join-Path $root 'config.json'
$exe = Join-Path $root 'VRPorts.exe'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw 'VRPorts.exe is missing. This source archive contains no executable; build the source first.'
}
$pwsh = Get-Command pwsh -ErrorAction SilentlyContinue
if (-not $pwsh) {
    throw 'PowerShell 7 (pwsh) is required for the Cyberpunk backend. Install it, then run this launcher again.'
}
if (-not (Test-Path -LiteralPath $configPath -PathType Leaf)) {
    Write-Host 'VR-CENTR first setup: Cyberpunk 2077 on Steam is the only current game adapter.'
    $suggestedGame = Join-Path ${env:ProgramFiles(x86)} 'Steam\steamapps\common\Cyberpunk 2077'
    $game = if (Test-Path -LiteralPath (Join-Path $suggestedGame 'bin\x64\Cyberpunk2077.exe')) { $suggestedGame } else { '' }
    if ($game) { Write-Host ('Found game: ' + $game) }
    else { $game = Read-Host 'Full path to your Steam Cyberpunk 2077 directory' }
    if (-not (Test-Path -LiteralPath (Join-Path $game 'bin\x64\Cyberpunk2077.exe') -PathType Leaf)) {
        throw 'Cyberpunk2077.exe was not found. Check the game directory and run setup again.'
    }
    $bundle = Read-Host 'Full path to your separately obtained Cyberpunk VR bundle'
    if (-not (Test-Path -LiteralPath (Join-Path $bundle 'bundle.json') -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $bundle 'stock-paths.json') -PathType Leaf)) {
        throw 'The VR bundle needs bundle.json and stock-paths.json. No configuration was written.'
    }
    $imagePackage = ''
    $sharedPackage = Join-Path $bundle 'shared-package.json'
    if (Test-Path -LiteralPath $sharedPackage -PathType Leaf) {
        $shared = Get-Content -LiteralPath $sharedPackage -Raw | ConvertFrom-Json
        if ($shared.schema -ne 1 -or $shared.scope -cne 'game-shared' -or
            $shared.nrStartupPolicy -cne 'off-until-manual') {
            throw 'The bundle shared-package.json has an unsupported schema, scope or NR startup policy. No configuration was written.'
        }
        $imagePackage = [IO.Path]::GetFullPath($sharedPackage)
    }
    $data = Join-Path $env:LOCALAPPDATA 'VR-CENTR\Cyberpunk2077'
    $vortex = Join-Path $env:ProgramFiles 'Vortex\Vortex.exe'
    $settings = [ordered]@{
        game = [IO.Path]::GetFullPath($game)
        bundle = [IO.Path]::GetFullPath($bundle)
        hold = (Join-Path $data 'user-first-launch.json')
        data = $data
        pwsh = $pwsh.Source
        vortex = $(if (Test-Path -LiteralPath $vortex) { $vortex } else { '' })
        vortexData = (Join-Path $env:APPDATA 'Vortex')
        gameProfilesEnabled = $true
        imagePackage = $imagePackage
    }
    $settings | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $configPath -Encoding UTF8
    Write-Host 'Local config.json created. It is excluded from Git.'
    Write-Host 'BEFORE applying a VR port: run the clean game to its first menu, exit it, then use the center action "Сохранить чистую основу".'
    Write-Host 'Create a clean flat PROFILE first. Return from VR by switching to that captured profile; this minimal bundle does not offer direct flat installer fallback.'
}
Start-Process -FilePath $exe -WorkingDirectory $root

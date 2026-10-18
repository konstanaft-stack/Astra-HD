$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
. (Join-Path $app 'GameLaunch.ps1')
. (Join-Path $app 'CyberpunkCollection.ps1')
function Check($Value,[string]$Why){if(-not $Value){throw $Why};Write-Output ('PASS: '+$Why)}
function Put([string]$Path,[string]$Text){[IO.Directory]::CreateDirectory((Split-Path $Path -Parent))|Out-Null;[IO.File]::WriteAllText($Path,$Text)}
$fixture=Join-Path $app ('audit/cyberpunk-collection-'+[guid]::NewGuid().ToString('N'))
$game="$fixture/game";$mo="$fixture/collection";$overwrite="$fixture/overwrite"
$config=@{game=$game;data="$fixture/data";mo2=@{root=$mo;modsRoot="$mo/mods";overwriteRoot=$overwrite}}
$plan=@{kind='mo2';game=$game;exe="$game/bin/x64/Cyberpunk2077.exe";profileRoot="$mo/profiles/Test";source=@{cyberpunk=@{enforceArchiveOrder=$true;reverseArchiveOrder=$false}}}
Put "$($plan.profileRoot)/modlist.txt" "+high`n-disabled`n+low"
Put "$mo/mods/low/archive/pc/mod/a.archive" 'low-a';Put "$mo/mods/low/archive/pc/mod/shared.archive" 'low-shared'
Put "$mo/mods/high/archive/pc/mod/b.archive" 'high-b';Put "$mo/mods/high/archive/pc/mod/shared.archive" 'high-shared'
Put "$mo/mods/disabled/archive/pc/mod/disabled.archive" 'disabled';Put "$overwrite/archive/pc/mod/z.archive" 'overwrite'
Put "$game/r6/cache/final.redscripts" 'original-cache'
Put "$game/r6/cache/metadata.json" 'original-metadata'
Put "$game/bin/x64/Cyberpunk2077.exe" 'game'
Put "$mo/mods/high/mods/red-high/info.json" '{}';Put "$mo/mods/low/mods/red-low/info.json" '{}'
$prep=Prepare-CyberpunkCollection $config $plan
Check ((Get-Content -LiteralPath "$overwrite/archive/pc/mod/modlist.txt") -join ',' -ceq 'a.archive,b.archive,shared.archive,z.archive') 'archive order follows source priority and de-duplicates the effective winner'
Check ([IO.File]::ReadAllText("$overwrite/r6/cache/final.redscripts") -ceq 'original-cache') 'base script cache copied to writable overlay'
Put "$overwrite/r6/cache/final.redscripts" 'compiled-mods'
$prep=Prepare-CyberpunkCollection $config $plan
Check ([IO.File]::ReadAllText("$overwrite/r6/cache/final.redscripts") -ceq 'compiled-mods' -and [IO.File]::ReadAllText("$game/r6/cache/final.redscripts") -ceq 'original-cache') 'repeat launch retains compiled profile cache and untouched base'
$plan.source.cyberpunk.reverseArchiveOrder=$true
$prep=Prepare-CyberpunkCollection $config $plan
Check ((Get-Content -LiteralPath "$overwrite/archive/pc/mod/modlist.txt") -join ',' -ceq 'z.archive,b.archive,shared.archive,a.archive') 'reverse archive order matches authored plugin option'
$plan.source.cyberpunk.enforceArchiveOrder=$false
Put "$overwrite/archive/pc/mod/modlist.txt" 'authored-order'
$prep=Prepare-CyberpunkCollection $config $plan
Check ([IO.File]::ReadAllText("$overwrite/archive/pc/mod/modlist.txt") -ceq 'authored-order') 'disabled enforcement preserves existing authored archive order'
Check ($prep.redmods -join ',' -ceq 'red-low,red-high') 'REDmod order comes from the same active mod list'
Put "$game/r6/cache/final.redscripts" 'new-game-cache'
$prep=Prepare-CyberpunkCollection $config $plan
Check ([IO.File]::ReadAllText("$overwrite/r6/cache/final.redscripts") -ceq 'new-game-cache') 'changed base cache replaces previous compiled profile cache'
Write-Output ('PASS: evidence '+$fixture)

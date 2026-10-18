param([switch]$BeforeMap,[string]$Baseline)
if($BeforeMap -and -not $Baseline){throw "-BeforeMap requires an explicit -Baseline source snapshot."}
$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
. (Join-Path $app 'GameProfileStore.ps1')
if($BeforeMap){
 $snapshot=[IO.File]::ReadAllText($Baseline)
 $definition=[regex]::Match($snapshot,'(?ms)^function ConvertTo-GameProfileManifest\(.*?(?=^function |\z)').Value
 . ([scriptblock]::Create($definition))
}
$fixture=Join-Path ([IO.Path]::GetTempPath()) ('profile-manifest-phase-'+[guid]::NewGuid().ToString('N'))
try{
 $root="$fixture/game";$store="$fixture/data/profile-store"
 [IO.Directory]::CreateDirectory("$store/objects")|Out-Null
 $bytes=[Text.Encoding]::UTF8.GetBytes('real verified bytes');$sha=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes));[IO.File]::WriteAllBytes("$store/objects/$sha",$bytes)
 $files=[Collections.Generic.List[object]]::new();$dirs=[Collections.Generic.List[object]]::new()
 foreach($i in 1..2000){[IO.Directory]::CreateDirectory("$root/mods/$i/a/b")|Out-Null;$files.Add(@{scope='game';path="mods/$i/a/b/file.json";sha256=$sha;size=$bytes.Length});foreach($path in @("mods/$i","mods/$i/a","mods/$i/a/b")){$dirs.Add(@{scope='game';path=$path})}}
 $config=@{data="$fixture/data";scopes=@(@{id='game';kind='directory';root=$root;requiredFiles=@('mods/1/a/b/file.json');exclusions=@(1..61|ForEach-Object {"archive/pc/content/base$_.archive"})})}
 $context=Get-GameProfileContext $config
 $manifest=@{schema=1;files=$files.ToArray();directories=$dirs.ToArray();scopes=@(@{id='game';kind='directory'})}
 $body=${function:ConvertTo-GameProfileManifest}.ToString()
 $body=$body.Replace('    $seen=@{};', '    $phase=[Diagnostics.Stopwatch]::StartNew();$seen=@{};')
 $body=$body.Replace('    $missing=@();', '    $script:metrics.files=$phase.Elapsed.TotalSeconds;$phase.Restart();$missing=@();')
 $body=$body.Replace('    foreach($entry in $files){', '    $script:metrics.rawDirectories=$phase.Elapsed.TotalSeconds;$phase.Restart();foreach($entry in $files){')
 $body=$body.Replace('    return @{schema=1;', '    $script:metrics.directoryNormalization=$phase.Elapsed.TotalSeconds;$phase.Restart();return @{schema=1;')
 $measure=[scriptblock]::Create($body)
 Start-GameProfileCasSession $context
 try{foreach($round in 1..2){$script:metrics=@{round=$round;beforeMap=[bool]$BeforeMap};$total=[Diagnostics.Stopwatch]::StartNew();$result=& $measure $context $manifest -RequireFiles;$total.Stop();$script:metrics.totalSeconds=$total.Elapsed.TotalSeconds;$script:metrics.casHashes=$context.casHashReads;$script:metrics.fileCount=$result.files.Count;$script:metrics.directoryCount=$result.directories.Count;$script:metrics|ConvertTo-Json -Compress}}finally{Stop-GameProfileCasSession $context}
}finally{if([IO.Path]::GetFullPath($fixture).StartsWith([IO.Path]::GetTempPath(),[StringComparison]::OrdinalIgnoreCase)){Remove-Item -LiteralPath $fixture -Recurse -Force}}

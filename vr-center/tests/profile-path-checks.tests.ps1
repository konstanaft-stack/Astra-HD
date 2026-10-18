param([Parameter(Mandatory)][string]$Baseline)
$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
# The exact comparison source is supplied explicitly.
. $baseline
$legacyRelative=${function:Assert-GameProfileRelativePath};$legacyPlain=${function:Assert-GameProfilePlainPath}
. (Join-Path $app 'GameProfileStore.ps1')
foreach($kind in @('file','directory','DIRECTORY')){foreach($path in @('', '.', '..','a','a/b/c.json','a//b','/a','C:a','a\b','a:','a*','a?','a"','a<','a>','a|',"a`n",'a.','a ','a/b.','con','CON.txt','com1','com0','com10','lpt9.ext','nul/x','auxiliary','a/...','a/ .','ü/д.json')){
 $old=$null;$new=$null
 try{& $legacyRelative $path $kind}catch{$old=$_.Exception.Message}
 try{Assert-GameProfileRelativePath $path $kind}catch{$new=$_.Exception.Message}
 if(($null -eq $old) -ne ($null -eq $new)){throw "Relative policy mismatch: [$path] kind=$kind old=$old new=$new"}
}}
$target=Join-Path ([IO.Path]::GetTempPath()) ('guard-absent-'+[guid]::NewGuid().ToString('N')+'/deep/file.json')
& $legacyPlain $target;Assert-GameProfilePlainPath $target
$oldTimer=[Diagnostics.Stopwatch]::StartNew();foreach($i in 1..2000){& $legacyRelative 'bin/x64/plugins/example/settings/deep/1.json'; & $legacyPlain $target};$oldTimer.Stop()
$newTimer=[Diagnostics.Stopwatch]::StartNew();foreach($i in 1..2000){Assert-GameProfileRelativePath 'bin/x64/plugins/example/settings/deep/1.json';Assert-GameProfilePlainPath $target};$newTimer.Stop()
$ratio=$newTimer.Elapsed.TotalSeconds/$oldTimer.Elapsed.TotalSeconds
@{oldSeconds=$oldTimer.Elapsed.TotalSeconds;newSeconds=$newTimer.Elapsed.TotalSeconds;ratio=$ratio}|ConvertTo-Json
if($ratio -gt .7){throw 'Fresh guard checks must improve at least 30 percent'}

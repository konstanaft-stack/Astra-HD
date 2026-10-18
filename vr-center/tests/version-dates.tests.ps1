$ErrorActionPreference='Stop'
. "$PSScriptRoot/../Core.ps1"
function Assert($v,$why){if(-not $v){throw $why}}
$root=Join-Path $PSScriptRoot ('fixture-dates-'+[guid]::NewGuid().ToString('N'))
$c=@{bundle="$root/bundle";data="$root/data";game="$root/game";hold="$root/hold.json";vortex="$root/no.exe";vortexData="$root/vortex"}
try{
New-Item -ItemType Directory -Force "$root/bundle/downloads","$root/data/snapshots"|Out-Null
$b=@{versions=@(@{id='old'},@{id='download';archiveSha256='AABB';upstreamOriginal=$true},@{id='download_original';archiveSha256='AABB';upstreamOriginal=$true})}
Write-Json "$root/bundle/downloads/a.download.json" @{sha256='aabb';downloadedUtc='2026-10-10T05:38:51Z'}
Write-Json "$root/data/snapshots/old.json" @{receipt=@{version='old';time='20261009-190000-000'}}
$receipt=@{version='download';time='20261010-094500-123'}
$dates=Get-PortVersionDates $c $b $receipt
Assert ($dates.download.downloadedUtc -like '2026-10-10T05:38:51*') 'Receipt download time lost'
Assert ($null -eq $dates.old.downloadedUtc) 'Unknown download time invented'
Assert ($dates.old.installedUtc -and $dates.download.installedUtc) 'Install time not recovered'
Assert ((Find-DownloadedVersion $b @{sha256='aabb'}) -eq 'download_original') 'Exact original import not selected'
Save-PortInstallTime $c $receipt
$dates=Get-PortVersionDates $c $b $null
Assert ($dates.download.installedUtc) 'Installation history lost after switching ports'
Write-Output 'PASS: exact receipt dates, missing dates stay unknown, historical installation retained, imported original selected by hash.'

# Shared ToFu archive dates are not build dates of our own variants.
$own=@{versions=@(
    @{id='tofu-aer-v1-candidate';family='Astra HD';label='Current';archiveSha256='AABB';builtUtc='2026-10-10T19:34:28.7924658Z';files=@{}},
    @{id='tofu-aer-x2-prepared-20261006';family='Astra HD';label='Other';archiveSha256='AABB';builtUtc='2026-10-11T12:30:00+04:00';files=@{}},
    @{id='own-unknown';family='Astra HD';label='Unknown';archiveSha256='AABB';importedUtc='2026-10-11T09:00:00Z';files=@{}},
    @{id='own-invalid';family='Astra HD';label='Invalid';archiveSha256='AABB';builtUtc='unconfirmed';files=@{}},
    @{id='tofu-original';family='ToFu';label='Original';archiveSha256='AABB';builtUtc='2026-10-11T10:00:00Z';files=@{}},
    @{id='import-original';family='ToFu';label='Imported';importedUtc='2026-10-09T09:00:00Z';files=@{}},
    @{id='saved-20261010-014557-d7f4bd';label='Сохранённая сборка • 10.10 01:45';kind='native';note='Точная копия файлов перед сменой: ToFu TE6 + наш AER v2 — кандидат. Дополнения уже входят в состав.';files=@{}},
    @{id='saved-20261010-102713-b15362';label='Сохранённая сборка • 10.10 10:27';kind='flat';note='Точная копия файлов перед сменой: Плоская игра. Дополнения уже входят в состав.';files=@{}}
)}
Write-Json "$root/bundle/bundle.json" $own
$state=Get-PortState $c
$current=$state.versions|Where-Object id -eq 'tofu-aer-v1-candidate'
Assert ($current.builtUtc -eq '2026-10-10T19:34:28.7924658Z') 'Confirmed build time lost at backend/UI boundary'
Assert ($null -eq ($state.versions|Where-Object id -eq 'own-invalid').builtUtc) 'Invalid build time accepted'
$saved=$state.versions|Where-Object id -eq 'saved-20261010-014557-d7f4bd'
Assert ($saved.family -eq 'Astra HD' -and $saved.sourceLabel -like '*наш AER*') 'Legacy Astra snapshot source hidden'
Assert ($saved.savedUtc -and -not $saved.builtUtc) 'Snapshot time confused with build time'
Assert (($state.versions|Where-Object id -eq 'saved-20261010-102713-b15362').family -eq 'Плоская игра') 'Flat snapshot misclassified'

# Execute the actual Port date formatter and ComboBox ItemsSource, without an Avalonia window.
$ui=[IO.File]::ReadAllText("$PSScriptRoot/../MainWindow.axaml.cs")
$portCode=[regex]::Match($ui,'(?s)public record Port\(.*?\r?\n\}').Value
$sortCode=[regex]::Match($ui,'ItemsSource=(.*?),SelectedItem=selected').Groups[1].Value
Assert ($portCode -and $sortCode) 'UI date/sort code not found'
$testCode=@"
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json.Nodes;
#nullable enable
$portCode
public static class VersionDateUiProbe {
 static string S(JsonNode? n,string key)=>n?[key]?.ToString()??"";
 public static Port[] Sort(List<Port> ports, Port selected)=>$sortCode;
 public static string[] Check(string json){
  var ports=JsonNode.Parse(json)!.AsArray().Select(n=>new Port(n!["id"]!.ToString(),n["label"]!.ToString(),n["family"]!.ToString(),n)).ToList();
  var current=ports.Single(p=>p.Id=="tofu-aer-v1-candidate");
  var original=ports.Single(p=>p.Id=="tofu-original");
  var imported=ports.Single(p=>p.Id=="import-original");
  var saved=ports.Single(p=>p.Id=="saved-20261010-014557-d7f4bd");
  return new[]{Sort(ports,current)[0].Id,current.DateLabel,ports.Single(p=>p.Id=="own-unknown").DateLabel,ports.Single(p=>p.Id=="own-invalid").DateLabel,original.DateLabel,imported.DateLabel,Sort(ports,original)[0].Id,saved.ToString()};
 }
}
"@
Add-Type -TypeDefinition $testCode
$actual=[VersionDateUiProbe]::Check(($state.versions|ConvertTo-Json -Depth 15 -AsArray))
Assert ($actual[0] -eq 'own-unknown') 'Newest added version must precede the old current candidate, comparing actual UTC times'
$buildLocal=([datetimeoffset]'2026-10-10T19:34:28.7924658Z').ToLocalTime().ToString('dd.MM.yyyy HH:mm')
Assert ($actual[1] -eq 'Собрано '+$buildLocal) 'Confirmed Astra build time is not labelled'
Assert ($actual[2] -eq ('Добавлено '+([datetimeoffset]'2026-10-11T09:00:00Z').ToLocalTime().ToString('dd.MM.yyyy HH:mm')) -and $actual[3] -eq 'дата не сохранена') 'Unknown build date invented or known import date hidden'
$downloadLocal=([datetimeoffset]'2026-10-10T05:38:51Z').ToLocalTime().ToString('dd.MM.yyyy HH:mm')
$importLocal=([datetimeoffset]'2026-10-09T09:00:00Z').ToLocalTime().ToString('dd.MM.yyyy HH:mm')
Assert ($actual[4] -eq ('Собрано '+([datetimeoffset]'2026-10-11T10:00:00Z').ToLocalTime().ToString('dd.MM.yyyy HH:mm')) -and $actual[5] -eq ('Добавлено '+$importLocal)) 'External port date provenance lost'
Assert ($actual[6] -eq 'tofu-original') 'External port version order changed'
Assert ($actual[7] -match '^Сохранено 10\.10\.2026 01:45 · ToFu TE6 \+ наш AER v2') 'Saved Astra timestamp/source not visible'
Write-Output 'PASS: build/save/import dates labelled, old Astra snapshot visible, newest addition first without a pinned candidate.'
}finally{
    $resolved=[IO.Path]::GetFullPath($root)
    if(-not $resolved.StartsWith([IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')+'\fixture-dates-',[StringComparison]::OrdinalIgnoreCase)){throw 'Unsafe date fixture cleanup path'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

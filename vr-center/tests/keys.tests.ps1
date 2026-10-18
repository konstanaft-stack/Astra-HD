$ErrorActionPreference='Stop'
. "$PSScriptRoot/../KeyBindings.ps1"
$p=Join-Path $PSScriptRoot ('fixture-'+[guid]::NewGuid().ToString('N'));[IO.Directory]::CreateDirectory($p)|Out-Null
$ini=Join-Path $p 'OptiScaler.ini';[IO.File]::WriteAllText($ini,"[Other]`nShortcutKey=9`n[Menu]`nShortcutKey = 0x75`nScale=1.3`n[DlssNr]`nEnabled=false`n")
if((Read-OptiMenuKey $ini) -ne 117){throw 'Wrong key read'}
Set-OptiMenuKey $ini 122
if((Read-OptiMenuKey $ini) -ne 122){throw 'Key not changed'}
$text=[IO.File]::ReadAllText($ini);if(-not $text.Contains('ShortcutKey=9') -or -not $text.Contains('Scale=1.3') -or -not $text.Contains('Enabled=false')){throw 'Unrelated settings changed'}
'PASS: actual OptiScaler Menu/ShortcutKey round trip; unrelated settings preserved.'

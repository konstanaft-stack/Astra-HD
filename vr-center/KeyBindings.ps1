. "$PSScriptRoot/Core.ps1"
function Read-OptiMenuKey([string]$Path){
 if(-not(Test-Path $Path)){return $null}
 $text=[IO.File]::ReadAllText($Path);$section=[regex]::Match($text,'(?ms)^\[Menu\][^\r\n]*\r?\n.*?(?=^\[|\z)')
 $m=[regex]::Match($section.Value,'(?m)^\s*ShortcutKey\s*=\s*(0x[0-9a-fA-F]+|[0-9]+|auto)\s*$')
 if(-not $m.Success){return $null};$v=$m.Groups[1].Value;if($v -eq 'auto'){return 45};if($v.StartsWith('0x')){return [Convert]::ToInt32($v.Substring(2),16)};return [int]$v
}
function Set-OptiMenuKey([string]$Path,[int]$Key){
 if($Key -notin @(19,45,117,118,119,120,121,122,123)){throw 'Неподдерживаемая клавиша.'}
 $text=[IO.File]::ReadAllText($Path);$section=[regex]::Match($text,'(?ms)^\[Menu\][^\r\n]*\r?\n.*?(?=^\[|\z)')
 if(-not $section.Success -or -not [regex]::IsMatch($section.Value,'(?m)^\s*ShortcutKey\s*=')){throw 'В OptiScaler.ini не найден Menu/ShortcutKey.'}
 $body=[regex]::Replace($section.Value,'(?m)^(\s*ShortcutKey\s*=\s*)[^\r\n]*',('${1}0x'+$Key.ToString('X2')))
 [IO.File]::WriteAllText($Path,$text.Substring(0,$section.Index)+$body+$text.Substring($section.Index+$section.Length),[Text.UTF8Encoding]::new($false))
}
function Save-PortKeys($Config,$Request){
 $b=Read-Json (Join-Path $Config.bundle 'bundle.json');if(-not @($b.versions|Where-Object {$_.id -eq $Request.version -and -not $_.archived}).Count){throw 'Порт не найден.'}
 $key=[int]$Request.optiKey;if($key -notin @(19,45,117,118,119,120,121,122,123)){throw 'Неподдерживаемая клавиша.'}
 $path=Join-Path $Config.data 'keybindings.json';$settings=Read-Json $path;if(-not $settings){$settings=@{ports=@{}}}
 $receipt=Read-Json (Join-Path $Config.game 'CPVR-Portable/installed.json');$ini=Join-Path $Config.game 'bin/x64/OptiScaler.ini'
 if($receipt.version -eq $Request.version -and $receipt.opti -and (Test-Path $ini)){
  if(Get-Process Cyberpunk2077 -ErrorAction SilentlyContinue){throw 'Для изменения привязки установленного OptiScaler сначала закрой игру.'}
  $backup=Join-Path $Config.data ('key-backups/'+[guid]::NewGuid().ToString('N')+'.ini');[IO.Directory]::CreateDirectory((Split-Path $backup -Parent))|Out-Null;Copy-Item $ini $backup;Set-OptiMenuKey $ini $key
  $message='Клавиша OptiScaler изменена. Вступит в силу при следующем запуске игры.'
 }else{$message='Клавиша сохранена для этого порта и будет применена при установке OptiScaler.'}
 $settings.ports[$Request.version]=@{optiKey=$key};Write-Json $path $settings;return $message
}

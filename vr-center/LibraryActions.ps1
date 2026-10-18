. "$PSScriptRoot/Core.ps1"
function Remove-PortVersions($Config,[string[]]$Ids){
 $gate=[Threading.Mutex]::new($false,'Local\IRD.SingleBuildTestDeploy.v1');$held=$false
 try{
  try{$held=$gate.WaitOne(0)}catch [Threading.AbandonedMutexException]{$held=$true}
  if(-not $held){throw 'Сейчас выполняется другая операция с библиотекой.'}
  $receipt=Read-Json (Join-Path $Config.game 'CPVR-Portable/installed.json');$path=Join-Path $Config.bundle 'bundle.json';$b=Read-Json $path
  $chosen=@();foreach($id in @($Ids|Sort-Object -Unique)){
   if($id -eq 'flat'){throw 'Плоская игра — базовый вариант, его нельзя удалить.'}
   if($receipt -and $id -eq $receipt.version){throw 'Эта сборка установлена. Сначала выбери и установи другую.'}
   $v=@($b.versions|Where-Object id -eq $id)
   if($v.Count -ne 1 -or $v[0].archived){throw 'Сборка не найдена в библиотеке.'};$chosen+=,$v[0]
  }
  if(-not $chosen.Count){throw 'Не выбрана сборка для удаления.'}
  $trash=Join-Path $Config.data ('library-trash/'+[guid]::NewGuid().ToString('N')+'.json')
  Write-Json $trash @{utc=[DateTime]::UtcNow.ToString('o');versions=$chosen;note='Shared payloads retained; game files untouched'}
  foreach($v in $chosen){$v.archived=$true;$v.available=$false}
  Write-Json $path $b
  return ('Удалено из библиотеки: '+$chosen.Count+'. Установленная игра не изменена.')
 }finally{if($held){$gate.ReleaseMutex()};$gate.Dispose()}
}

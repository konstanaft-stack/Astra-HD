$ErrorActionPreference='Stop'
. (Join-Path (Split-Path $PSScriptRoot -Parent) 'GameProfileStore.ps1')
$fixture=Join-Path ([IO.Path]::GetTempPath()) ('profile-cas-lease-'+[guid]::NewGuid().ToString('N'))
try{
 [IO.Directory]::CreateDirectory("$fixture/objects")|Out-Null
 $bytes=[Text.Encoding]::UTF8.GetBytes('exact immutable CAS bytes');$sha=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes));$path="$fixture/objects/$sha"
 [IO.File]::WriteAllBytes($path,$bytes);$context=@{store=$fixture}
 $writer=[IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::ReadWrite)
 Start-GameProfileCasSession $context
 try{$denied=$false;try{Assert-GameProfileCasObject $context $path $sha $bytes.Length}catch{$denied=$true};if(-not $denied){throw 'Existing writer did not abort lease admission'}}finally{Stop-GameProfileCasSession $context;$writer.Dispose()}
 Start-GameProfileCasSession $context
 try{
  Assert-GameProfileCasObject $context $path $sha $bytes.Length
  Assert-GameProfileCasObject $context $path $sha $bytes.Length
  if($context.casHashReads -ne 1){throw 'Same immutable object must be hashed once per operation'}
  $denied=$false;try{[IO.File]::WriteAllText($path,'foreign')}catch{$denied=$true};if(-not $denied){throw 'Leased CAS allowed a writer'}
  $denied=$false;try{[IO.File]::Delete($path)}catch{$denied=$true};if(-not $denied){throw 'Leased CAS allowed deletion'}
  $denied=$false;try{Assert-GameProfileCasObject $context $path $sha ($bytes.Length+1)}catch{$denied=$true};if(-not $denied){throw 'Wrong size accepted on cache hit'}
  $moved=$false;try{[IO.Directory]::Move("$fixture/objects","$fixture/moved");$moved=$true}catch{}
  if($moved){
   [IO.Directory]::CreateDirectory("$fixture/objects")|Out-Null;[IO.File]::WriteAllBytes($path,$bytes)
   $denied=$false;try{Assert-GameProfileCasObject $context $path $sha $bytes.Length}catch{$denied=$true};if(-not $denied){throw 'Renamed parent allowed stale handle reuse'}
  }else{Assert-GameProfileCasObject $context $path $sha $bytes.Length}
 }finally{Stop-GameProfileCasSession $context}
 if($moved){[IO.File]::WriteAllText("$fixture/moved/$sha",'released')}
 [IO.File]::WriteAllText($path,'corruption')
 Start-GameProfileCasSession $context
 try{$denied=$false;try{Assert-GameProfileCasObject $context $path $sha 10}catch{$denied=$true};if(-not $denied){throw 'First corrupt CAS accepted'}}finally{Stop-GameProfileCasSession $context}
 [IO.File]::Delete($path)
 'PASS: once per operation SHA, writer/delete exclusion, size guard, parent rename rejection, corruption rejection and handle cleanup.'
}finally{if([IO.Path]::GetFullPath($fixture).StartsWith([IO.Path]::GetTempPath(),[StringComparison]::OrdinalIgnoreCase)){Remove-Item -LiteralPath $fixture -Recurse -Force}}

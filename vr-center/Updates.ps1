. "$PSScriptRoot/Core.ps1"
function Get-CachedPortUpdates($Config){
 $cache=Read-Json (Join-Path $Config.data 'updates/ports.json');if(-not $cache){return $null}
 $bundle=Read-Json (Join-Path $Config.bundle 'bundle.json')
 foreach($repo in $cache.repositories){$tags=@(@($repo.local)+@($bundle.versions|Where-Object repository -eq $repo.repository|ForEach-Object tag)|Where-Object {$_}|Sort-Object -Unique);$repo.localTags=$tags;$repo.local=$tags -join ', ';$repo.hasUpdate=($repo.releases.Count -gt 0 -and $repo.releases[0].tag -notin $tags)}
 return $cache
}
function Get-PortUpdates($Config,[string]$RepoId=''){
 . (Join-Path $Config.bundle 'Update-Ports.ps1') -LibraryOnly
 $repos=@(Read-Repositories);if($RepoId){$repos=@($repos|Where-Object id -eq $RepoId);if($repos.Count -ne 1){throw 'Для этого порта не настроен репозиторий GitHub.'}}
 $rows=@();foreach($repo in $repos){
  try{$releases=@(Get-RepositoryReleases $repo);$localIds=@((Read-Json (Join-Path $Config.bundle 'bundle.json')).versions|ForEach-Object {$_.releaseTag})
   $rows+=@{id=$repo.id;name=$repo.name;repository=$repo.repository;local=$repo.cachedVersion;error=$null;releases=@($releases|ForEach-Object {@{tag=$_.tag_name;date=$_.published_at;prerelease=$_.prerelease;url=$_.html_url;assets=@($_.assets|Where-Object name -match '\.(zip|exe)$'|ForEach-Object {@{id=$_.id;name=$_.name;size=$_.size;url=$_.browser_download_url;digest=$_.digest}})}})}
  }catch{$rows+=@{id=$repo.id;name=$repo.name;repository=$repo.repository;local=$repo.cachedVersion;error=$_.Exception.Message;releases=@()}}
 }
 if($RepoId){$old=Read-Json (Join-Path $Config.data 'updates/ports.json');if($old){$rows=@($old.repositories|Where-Object id -ne $RepoId)+$rows}}
 $result=@{checkedUtc=[DateTime]::UtcNow.ToString('o');repositories=$rows};Write-Json (Join-Path $Config.data 'updates/ports.json') $result;return $result
}
function Download-PortUpdate($Config,$Request){
 . (Join-Path $Config.bundle 'Update-Ports.ps1') -LibraryOnly
 $cache=Read-Json (Join-Path $Config.data 'updates/ports.json')
 $repo=@($cache.repositories|Where-Object id -eq $Request.repoId)[0]
 $release=@($repo.releases|Where-Object tag -eq $Request.tag)[0]
 $asset=@($release.assets|Where-Object id -eq $Request.assetId)[0]
 if(-not $repo -or -not $release -or -not $asset){throw 'Сначала проверь обновления и выбери файл релиза.'}
 $saved=Save-ReleaseAsset ([pscustomobject]@{repoId=$repo.id;repository=$repo.repository;tag=$release.tag;assetName=$asset.name;assetId=$asset.id;url=$asset.url;size=$asset.size;digest=$asset.digest})
 $saved|Add-Member -NotePropertyName repoId -NotePropertyValue $repo.id -Force
 Write-Json (Join-Path $Config.data 'updates/last-download.json') $saved
 return $saved
}
function Get-GitBlobHash([string]$Path){
 $f=[IO.File]::OpenRead($Path);$sha=[Security.Cryptography.SHA1]::Create()
 try{$header=[Text.Encoding]::UTF8.GetBytes('blob '+$f.Length+[char]0);$null=$sha.TransformBlock($header,0,$header.Length,$header,0);$buffer=[byte[]]::new(1048576);while(($n=$f.Read($buffer,0,$buffer.Length)) -gt 0){$null=$sha.TransformBlock($buffer,0,$n,$buffer,0)};$null=$sha.TransformFinalBlock([byte[]]::new(0),0,0);return ([BitConverter]::ToString($sha.Hash)).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose();$f.Dispose()}
}
function Update-NvidiaLibraries($Config){
 $headers=@{'User-Agent'='VRPorts/2.1';Accept='application/vnd.github+json'}
 $release=Invoke-RestMethod 'https://api.github.com/repos/NVIDIA/DLSS/releases/latest' -Headers $headers -TimeoutSec 30
 $tag=[string]$release.tag_name;if($tag -notmatch '^v?[0-9]+\.[0-9]+\.[0-9]+$'){throw 'Неподдерживаемая метка NVIDIA SDK.'}
 $items=Invoke-RestMethod ('https://api.github.com/repos/NVIDIA/DLSS/contents/lib/Windows_x86_64/rel?ref='+[Uri]::EscapeDataString($tag)) -Headers $headers -TimeoutSec 30
 $known=Read-Json (Join-Path $Config.data 'library-catalog.json');$entries=if($known){@($known.entries)}else{@()}
 $bundle=Read-Json (Join-Path $Config.bundle 'bundle.json');$result=@()
 foreach($file in @('nvngx_dlss.dll','nvngx_dlssg.dll','nvngx_dlssd.dll')){
  $item=@($items|Where-Object name -eq $file)[0];if(-not $item){throw "В NVIDIA SDK отсутствует $file"}
  $expected="https://raw.githubusercontent.com/NVIDIA/DLSS/$tag/lib/Windows_x86_64/rel/$file"
  if($item.download_url -ne $expected -or $item.sha -notmatch '^[a-f0-9]{40}$'){throw 'Неожиданный источник NVIDIA.'}
  $hashes=@(@($bundle.versions|ForEach-Object {$_.files['bin/x64/'+$file]})+@($entries|Where-Object file -eq $file|ForEach-Object hash)|Where-Object {$_}|Sort-Object -Unique)
  $source=$null;foreach($h in $hashes){$p=Join-Path $Config.bundle "files/$h";if((Test-Path $p) -and (Get-Item $p).Length -eq $item.size -and (Get-GitBlobHash $p) -eq $item.sha){$source=$p;break}}
  if(-not $source){$source=Join-Path $Config.data ("updates/$tag-$file");[IO.Directory]::CreateDirectory((Split-Path $source -Parent))|Out-Null;Invoke-WebRequest $expected -Headers $headers -OutFile $source -TimeoutSec 180}
  if((Get-Item $source).Length -ne $item.size -or (Get-GitBlobHash $source) -ne $item.sha){throw "Контрольная сумма GitHub не совпала: $file"}
  $hash=(Get-FileHash $source).Hash;$version=([string][Diagnostics.FileVersionInfo]::GetVersionInfo($source).FileVersion).Replace(',','.')
  if(-not $version){throw 'Скачанный файл не содержит версии DLL.'}
  $object=Join-Path $Config.bundle "files/$hash";if(-not(Test-Path $object)){Copy-Item -LiteralPath $source -Destination $object}
  $entry=@{file=$file;hash=$hash;version=$version;tag=$tag;source=$expected;gitBlob=$item.sha;checkedUtc=[DateTime]::UtcNow.ToString('o')}
  $entries=@($entries|Where-Object { -not ($_.file -eq $file -and $_.hash -eq $hash) })+@($entry);$result+=$entry
 }
 Write-Json (Join-Path $Config.data 'library-catalog.json') @{entries=$entries}
 return $result
}

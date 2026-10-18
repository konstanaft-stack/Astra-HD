[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
# snapshot is the current maintained backend, including GPU timing, fenced
# progressive output and single-reset priming. The historical donor generator
# predates those changes: it must not overwrite this source on adapter refresh.
$taskDest=Join-Path $PSScriptRoot 'snapshot'
$taskNative=Join-Path $taskDest 'nv_backend.c'
$taskImpl=Join-Path $taskDest 'nv_backend_impl.inc'
$taskText=Get-Content -LiteralPath $taskNative -Raw
$taskImplText=Get-Content -LiteralPath $taskImpl -Raw
foreach($taskContract in @('gfx_enqueue_closed','gfx_wait_for')){
 if(-not $taskText.Contains($taskContract)){throw "Current maintained backend missing $taskContract; restore its exact source checkpoint"}
}
foreach($taskContract in @('fg_nv_run_ready','fg_nv_enable_gpu_timing','nv_batch(e,previous,1,1,NULL,NULL)')){
 if(-not $taskImplText.Contains($taskContract)){throw "Current maintained backend missing $taskContract; refusing historical regeneration"}
}
$taskLoaderPattern='(?s)static HMODULE ngx_dll;\s*static void load_ngx\(int shared_core\).*?\r?\n}\r?\n'
$taskLoaders=[regex]::Matches($taskText,$taskLoaderPattern)
if($taskLoaders.Count -ne 1){throw 'Current loader layout changed; preserve the shared-core lifetime contract'}
$taskLoader=$taskLoaders[0]
$taskSharedLoader=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'shared_ngx_loader.inc') -Raw
$taskText=$taskText.Substring(0,$taskLoader.Index)+$taskSharedLoader+$taskText.Substring($taskLoader.Index+$taskLoader.Length)
$taskCreate='NGXCHK(NGX_D3D12_CREATE_DLSSG(cmd, 1, 1, &fg_feature, eval_params, &cp), "CREATE_DLSSG");'
if(-not $taskText.Contains($taskCreate)){throw 'CreateFeature layout changed; cannot omit loaded-module verification'}
if(-not $taskText.Contains('verify_loaded_snippets(0);')){
 $taskText=$taskText.Replace($taskCreate,"verify_loaded_snippets(0);`n    $taskCreate`n    verify_loaded_snippets(1);")
}
$taskCountGuard='if(current->source_f<=previous->source_f||current->source_f-previous->source_f!=2)'
if(-not $taskImplText.Contains($taskCountGuard)){throw 'Current native count guard layout changed'}
if(-not $taskImplText.Contains('if(cpvr_native_x2_only&&count!=1)')){
 $taskImplText=$taskImplText.Replace($taskCountGuard,"if(cpvr_native_x2_only&&count!=1)return nv_reject(`"current NVIDIA profile qualified for native X2 only`");`n $taskCountGuard")
}
$taskImplText=$taskImplText.Replace('snprintf(nv_status.message,sizeof(nv_status.message),"backend attached; %s",initialize_ngx?"owned NGX init":"shared pinned core 91D3742F; no additional Init");',
 'snprintf(nv_status.message,sizeof(nv_status.message),"backend attached; profile=%s; %s",cpvr_profile_name,initialize_ngx?"owned NGX init":"shared pinned core; no additional Init");')
if(-not $taskImplText.Contains('release_snippet_refs();')){
 $taskRelease='if(ngx_dll)FreeLibrary(ngx_dll);'
 if(-not $taskImplText.Contains($taskRelease)){throw 'Current module retirement layout changed'}
 $taskImplText=$taskImplText.Replace($taskRelease,"$taskRelease`n release_snippet_refs();")
}
[IO.File]::WriteAllText($taskNative,$taskText,[Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText($taskImpl,$taskImplText,[Text.UTF8Encoding]::new($false))
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'native_identity.h') -Destination (Join-Path $taskDest 'native_identity.h') -Force
Get-ChildItem -LiteralPath $taskDest -Recurse -File | ForEach-Object {
 [ordered]@{file=$_.FullName;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'SNAPSHOT-HASHES.json') -Encoding utf8
Write-Output 'Refreshed exact identity/loader adapters; maintained GPU/ready/priming source preserved'
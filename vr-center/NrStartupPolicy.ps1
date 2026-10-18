# Explicit shared-package launch policy. The author hotkey remains a runtime choice.
function Invoke-NrStartupPolicy($Config){
    $spec=Get-SharedEnhancementDefinition $Config
    if(-not $spec -or -not $spec.nrStartupPolicy){return}
    if($spec.nrStartupPolicy -cne 'off-until-manual'){throw 'Unknown NR startup policy.'}
    $state=Get-SharedEnhancementState $Config $spec
    if(-not $state.installed -or -not $state.enabled){return}
    if(-not $state.healthy){throw 'Shared enhancement is not healthy.'}
    Assert-GameWritable $Config
    $relative='bin/x64/OptiScaler.ini'
    if(@($spec.files|Where-Object {$_.path -ceq $relative -and $_.mutable}).Count -ne 1){throw 'NR startup policy requires the shared mutable OptiScaler.ini.'}
    $path=Resolve-ChildPath $Config.game $relative
    $bytes=[IO.File]::ReadAllBytes($path)
    if($bytes -contains [byte]0){throw 'Unsupported NR INI encoding.'}
    # Latin1 is a reversible byte mapping: preserve BOM, Unicode and all unrelated bytes.
    $offset=if($bytes.Length -ge 3 -and $bytes[0] -eq 239 -and $bytes[1] -eq 187 -and $bytes[2] -eq 191){3}else{0}
    $text=[Text.Encoding]::Latin1.GetString($bytes,$offset,$bytes.Length-$offset)
    $sections=[regex]::Matches($text,'(?im)^[ \t]*\[DlssNr\][ \t]*(?:[;#][^\r\n]*)?\r?$')
    if($sections.Count -ne 1){throw 'NR INI requires exactly one DlssNr section.'}
    $start=$sections[0].Index+$sections[0].Length
    $next=[regex]::Match($text.Substring($start),'(?m)^[ \t]*\[')
    $length=if($next.Success){$next.Index}else{$text.Length-$start}
    $section=$text.Substring($start,$length)
    $keys=[regex]::Matches($section,'(?im)^[ \t]*Enabled[ \t]*=[^\r\n]*')
    if($keys.Count -ne 1){throw 'NR INI requires exactly one Enabled key.'}
    $value=[regex]::Match($keys[0].Value,'(?i)^[ \t]*Enabled[ \t]*=[ \t]*(true|false)(?=[ \t]*(?:[;#].*)?$)')
    if(-not $value.Success){throw 'Invalid NR Enabled value.'}
    if($value.Groups[1].Value -ieq 'false'){return}
    $at=$start+$keys[0].Index+$value.Groups[1].Index
    $updated=$text.Substring(0,$at)+'false'+$text.Substring($at+4)
    $payload=[byte[]](@($bytes|Select-Object -First $offset)+@([Text.Encoding]::Latin1.GetBytes($updated)))
    $request=Join-Path $Config.data ('requests/nr-startup-'+[guid]::NewGuid().ToString('N')+'.ini')
    [IO.Directory]::CreateDirectory((Split-Path $request -Parent))|Out-Null
    try{
        [IO.File]::WriteAllBytes($request,$payload)
        Assert-GameWritable $Config
        if((Get-FileHash -LiteralPath $path).Hash -cne [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes))){throw 'NR INI changed during launch preflight.'}
        $null=Invoke-ComponentTransaction $Config @(@{path=$relative;source=$request;sha256=(Get-FileHash -LiteralPath $request).Hash})
    }finally{if(Test-Path -LiteralPath $request){[IO.File]::Delete($request)}}
}

# Build-owned contract. No writes, preset consent or general INI defaults.
function Read-NativeRequiredIniText([string]$Path){
    # GetString retains a UTF-8 BOM as U+FEFF; the existing UTF-8 writer then
    # preserves those same bytes. Unsupported encodings fail before any write.
    return [Text.UTF8Encoding]::new($false,$true).GetString([IO.File]::ReadAllBytes($Path))
}
function Get-NativeRequiredIniPolicy($Version){
    $policy=$Version.requiredIni
    if($null -eq $policy){return $false}
    $keys=@(if($policy -is [Collections.IDictionary]){$policy.Keys}else{$policy.PSObject.Properties.Name})
    $values=$policy.values
    $names=@(if($values -is [Collections.IDictionary]){$values.Keys}else{$values.PSObject.Properties.Name})
    $schema=$policy.schema;$value=$values.xr_hud_to_second_eye
    $schemaType=$schema -is [int] -or $schema -is [long]
    $valueType=$value -is [int] -or $value -is [long] -or $value -is [string]
    if($Version.kind -cne 'native' -or $keys.Count -ne 2 -or $keys -cnotcontains 'schema' -or $keys -cnotcontains 'values' -or
       -not $schemaType -or $schema -ne 1 -or $names.Count -ne 1 -or $names[0] -cne 'xr_hud_to_second_eye' -or
       -not $valueType -or [string]$value -cne '1'){throw 'Invalid native requiredIni contract'}
    return $true
}
function Update-NativeRequiredIniText([string]$Text,$Version){
    if(-not(Get-NativeRequiredIniPolicy $Version)){return $Text}
    if($Text.Contains([char]0)){throw 'Invalid native INI text'}
    $offset=if($Text.StartsWith([string][char]0xFEFF)){1}else{0}
    $body=$Text.Substring($offset)
    if($body -match '(?m)^[ \t]*\['){throw 'Required native INI must use flat keys'}
    $keys=[regex]::Matches($body,'(?im)^[ \t]*xr_hud_to_second_eye[ \t]*=')
    if($keys.Count -gt 1){throw 'Duplicate required native INI key'}
    if($keys.Count -eq 1){
        $matches=[regex]::Matches($body,'(?im)^[ \t]*xr_hud_to_second_eye[ \t]*=[ \t]*(?<value>[01])[ \t]*(?:[;#][^\r\n]*)?(?=\r?$)')
        if($matches.Count -ne 1){throw 'Invalid required native INI value'}
        $v=$matches[0].Groups['value'];$at=$offset+$v.Index
        return $Text.Substring(0,$at)+'1'+$Text.Substring($at+$v.Length)
    }
    $newline=if($body.Contains([string][char]13+[char]10)){[string][char]13+[char]10}else{[string][char]10}
    $separator=if(-not$body.Length -or $body.EndsWith([string][char]10)){''}else{$newline}
    return $Text+$separator+'xr_hud_to_second_eye=1'+$newline
}

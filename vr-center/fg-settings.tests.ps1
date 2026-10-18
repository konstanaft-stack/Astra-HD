param([Parameter(Mandatory)][string]$Installer,[Parameter(Mandatory)][string]$SettingsFixture)
$ErrorActionPreference='Stop'
$t=$null;$e=$null;$ast=[Management.Automation.Language.Parser]::ParseFile($Installer,[ref]$t,[ref]$e)
if($e.Count){throw ($e|Out-String)}
$f=$ast.Find({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Native-FG'},$true)
. ([scriptblock]::Create($f.Extent.Text))
$source=$SettingsFixture
$original=[IO.File]::ReadAllText($source)
$off=Native-FG $original $false
if($off -cne $original){throw 'Disabled settings changed unrelated bytes'}
$on=Native-FG $original $true
$j=$on|ConvertFrom-Json
$options=@($j.data|ForEach-Object options)
if(($options|Where-Object name -eq FrameGeneration).value -ne 'DLSS'){throw 'FG enable failed'}
if(($options|Where-Object name -eq FrameGeneration).index -ne 1){throw 'FG index failed'}
if(($options|Where-Object name -eq DLSSFrameGen).value -ne $true){throw 'DLSS flag failed'}
if((Native-FG $on $false) -cne $original){throw 'Roundtrip altered unrelated settings'}
$standard='{"data":[{"options":[{"name":"FrameGeneration","value":"Off","index":0},{"name":"DLSSFrameGen","value":false}]}]}'
$null=Native-FG $standard $true
foreach($bad in @($standard.Replace('FrameGeneration','MissingGeneration'),$standard.Replace('"index":0','"other":0'),'{invalid}',$standard.Replace('{"name":"DLSSFrameGen","value":false}','{"name":"DLSSFrameGen","value":false},{"name":"DLSSFrameGen","value":false}'))){
    $threw=$false;try{$null=Native-FG $bad $true}catch{$threw=$true};if(-not $threw){throw 'Malformed/ambiguous input accepted'}
}
'PASS: real reordered settings, both FG values, exact unrelated-byte preservation, name-first JSON, missing/duplicate/invalid rejection.'

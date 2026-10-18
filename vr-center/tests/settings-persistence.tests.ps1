param([Parameter(Mandatory)][string]$Installer,[Parameter(Mandatory)][string]$SettingsFixture)
$ErrorActionPreference='Stop'
# Explicit installer source is parsed; never installed by this test.
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($installer,[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'Installer parse failed'}
foreach($name in @('Native-FG','Installation-Settings')){
    $fn=$ast.Find({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name},$true)
    if(-not $fn){throw "Missing $name"};. ([scriptblock]::Create($fn.Extent.Text))
}
$source=[IO.File]::ReadAllText($SettingsFixture)
$enabled=Native-FG $source $true
foreach($sample in @($source,$enabled)){
    foreach($flag in @($false,$true)){
        if((Installation-Settings $sample 'native' $flag) -cne $sample){throw 'Native port changed player settings'}
    }
}
if((Installation-Settings $source 'flat' $true) -cne $enabled){throw 'Explicit flat FG enable regressed'}
if((Installation-Settings $enabled 'luke' $true) -cne (Native-FG $enabled $false)){throw 'Legacy external-FG route changed'}
$plan=$ast.FindAll({param($n)$n -is [Management.Automation.Language.AssignmentStatementAst] -and $n.Left.Extent.Text -eq '$desiredSettings'},$true)
if($plan.Count -ne 1 -or $plan[0].Right.Extent.Text -notmatch '^Installation-Settings '){throw 'Installation bypasses tested settings policy'}
'PASS: native FG on/off preserved byte-for-byte with graphics/input/audio; explicit flat/legacy policies retained; install uses policy.'

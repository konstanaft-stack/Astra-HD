$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$model=Join-Path $root 'UIcontrol/FeatureToggleState.cs'
if(Test-Path -LiteralPath $model){$source=[IO.File]::ReadAllText($model)}else{
    # Execute the existing SwitchCard colour decision for the regression RED.
    $ui=[IO.File]::ReadAllText((Join-Path $root 'MainWindow.axaml.cs'))
    $legacy=[regex]::Match($ui,'return (alternateName!=null\|\|actual\?\("","#86EFAC"\):\("","#E88D85"\));').Groups[1].Value
    if(-not $legacy){throw 'Existing SwitchCard decision was not found'}
    $source=@"
using System;
namespace VRPorts {
 public enum FeatureVisualState { On, Off, Pending, Unavailable }
 public record FeatureToggleState(string Name,string? AlternateName,bool Selected,bool Actual,bool Available) {
  public string DisplayName=>AlternateName!=null&&!Selected?AlternateName:Name;
  public FeatureVisualState VisualState { get { var alternateName=AlternateName;var actual=Actual;var old=$legacy;return old.Item2=="#86EFAC"?FeatureVisualState.On:FeatureVisualState.Off; } }
 }
 public sealed class FeatureToggleBinding {
  readonly Func<FeatureToggleState> read; readonly Action<bool> write;
  public FeatureToggleBinding(Func<FeatureToggleState> read,Action<bool> write){this.read=read;this.write=write;}
  public FeatureToggleState State=>read();
  public bool RequestToggle(bool blocked=false){var s=read();if(blocked||!s.Available)return false;write(!s.Selected);return true;}
 }
}
"@
}
$test=@'
namespace FeatureToggleTests {
using System;
using VRPorts;
public static class FeatureToggleStateTests {
 static void Equal<T>(T expected,T actual,string why){if(!Equals(expected,actual))throw new Exception(why+": expected "+expected+", got "+actual);}
 static FeatureToggleState State(bool selected,bool actual,bool available=true,string? alternate=null)=>new("Feature",alternate,selected,actual,available);
 public static void Run(){
  Equal(FeatureVisualState.On,State(true,true).VisualState,"actual enabled");
  Equal(FeatureVisualState.Off,State(false,false).VisualState,"actual disabled");
  Equal(FeatureVisualState.Pending,State(true,false).VisualState,"staged enable must await Apply");
  Equal(FeatureVisualState.Pending,State(false,true).VisualState,"staged disable must await Apply");
  Equal(FeatureVisualState.Unavailable,State(true,true,false).VisualState,"unavailable wins over actual enabled");
  Equal(FeatureVisualState.Unavailable,State(false,true,false).VisualState,"unavailable wins over pending");
  Equal(FeatureVisualState.On,State(false,false,true,"Alternative").VisualState,"actual alternative mode remains active");
  Equal("Alternative",State(false,false,true,"Alternative").DisplayName,"alternative mode name");
  Equal("Feature",State(true,true,true,"Alternative").DisplayName,"primary mode name");
  Equal(FeatureVisualState.Pending,State(false,true,true,"Alternative").VisualState,"draft alternative mode is pending");

  bool selected=false,actual=false;int writes=0;
  var staged=new FeatureToggleBinding(()=>State(selected,actual),v=>{writes++;selected=v;});
  Equal(true,staged.RequestToggle(),"staged click accepted");
  Equal(FeatureVisualState.Pending,staged.State.VisualState,"click stages enable");
  Equal(false,staged.State.Actual,"staged click cannot claim installed");
  actual=selected;
  Equal(FeatureVisualState.On,staged.State.VisualState,"Apply response resolves pending");
  Equal(false,staged.RequestToggle(true),"busy click ignored");
  Equal(1,writes,"busy click has no side effect");

  bool backend=false;bool? requested=null;
  var immediate=new FeatureToggleBinding(()=>State(backend,backend),v=>requested=v);
  Equal(true,immediate.RequestToggle(),"immediate request accepted");
  Equal(true,requested,"requested USB state");
  Equal(FeatureVisualState.Off,immediate.State.VisualState,"USB cannot turn green before backend success");
  backend=true;
  Equal(FeatureVisualState.On,immediate.State.VisualState,"fresh backend response turns USB green");
  immediate.RequestToggle();
  Equal(false,requested,"requested USB disable");
  Equal(FeatureVisualState.On,immediate.State.VisualState,"failed USB disable keeps actual enabled");

  var failed=new FeatureToggleBinding(()=>State(false,false),v=>throw new InvalidOperationException("backend rejected"));
  bool threw=false;try{failed.RequestToggle();}catch(InvalidOperationException){threw=true;}
  Equal(true,threw,"request failure exposed to caller");
  Equal(FeatureVisualState.Off,failed.State.VisualState,"failed request cannot alter actual state");
  var unavailable=new FeatureToggleBinding(()=>State(false,false,false),v=>writes++);
  Equal(false,unavailable.RequestToggle(),"disabled click ignored");
  Equal(1,writes,"disabled click has no side effect");
 }
}
}
'@
# Compile the framework-free production state/binding and execute its behavior.
Add-Type -TypeDefinition ($source+"`n"+$test) -CompilerOptions '/nullable:enable'
[FeatureToggleTests.FeatureToggleStateTests]::Run()
'PASS: actual/on/off, staged enable/disable, unavailable, alternate names, Apply response, busy/disabled clicks, USB success/failure.'

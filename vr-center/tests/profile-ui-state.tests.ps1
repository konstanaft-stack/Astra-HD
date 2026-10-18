$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$model=Join-Path $root 'GameProfileControls.cs'
if(-not (Test-Path -LiteralPath $model)){throw 'RED: profile UI state model does not exist yet'}
$source=([IO.File]::ReadAllText($model) -split '// PROFILE_UI_CONTROLS',2)[0]
$tests=@'
namespace ProfileUiTests {
 using System;
 using VRPorts;
 public static class Checks {
  static void Equal<T>(T expected,T actual,string why){if(!Equals(expected,actual))throw new Exception(why+": expected "+expected+", got "+actual);}
  public static void Run(){
   var model=new GameProfileSelection();
   var created=new GameProfileChoice("dated","Сохранённый",false,"2026-10-11T10:00:00Z");
   Equal(true,created.CreatedTooltip.Contains("2026"),"created tooltip uses backend createdUtc");
   Equal("Дата создания ещё не записана",new GameProfileChoice("current","Текущий",false).CreatedTooltip,"virtual date is not invented");
   model.SetSnapshot(true,"A",new[]{new GameProfileChoice("A","Current",true),new GameProfileChoice("B","Other",false)});
   Equal("A",model.ActiveId,"snapshot owns actual profile");
   Equal(false,model.CanDelete("A"),"active cannot be removed");
   Equal(true,model.CanDelete("B"),"inactive can be removed");
   Equal(true,model.BeginSwitch("B"),"inactive switch starts");
   Equal("A",model.ActiveId,"pending choice is not actual");
   Equal("B",model.PendingId,"requested target is tracked");
   Equal(GameProfileMarker.Active,model.Marker("A"),"actual marker remains green during switch");
   Equal(GameProfileMarker.Pending,model.Marker("B"),"pending marker is separate blue");
   Equal(false,model.CanDelete("B"),"pending action disables deletion");
   Equal(false,model.BeginSwitch("A"),"no parallel switch");
   model.CancelPending();
   Equal("A",model.ActiveId,"failure preserves actual profile");
   Equal(GameProfileMarker.None,model.Marker("B"),"failure clears pending marker");
   Equal(true,model.CanDelete("B"),"failure re-enables inactive removal");
   Equal(true,model.BeginSwitch("B"),"retry allowed");
   model.SetSnapshot(true,"B",new[]{new GameProfileChoice("A","Current",true),new GameProfileChoice("B","Other",false)});
   Equal("B",model.ActiveId,"only backend success changes actual");
   Equal(null,model.PendingId,"successful snapshot clears pending");
   Equal(false,model.CanDelete("B"),"new actual cannot be removed");
   Equal(false,model.BeginSwitch("missing"),"unknown profile is rejected");
   model.SetSnapshot(false,"B",model.Profiles);
   Equal(false,model.CanDelete("A"),"unavailable disables mutation");
   Equal(false,model.BeginSwitch("A"),"unavailable disables switch");
   model.SetSnapshot(false,"",Array.Empty<GameProfileChoice>());
   Equal(GameProfileMarker.None,model.Marker("current"),"unavailable snapshot does not invent active current marker");
   model.SetSnapshot(true,"current",new[]{new GameProfileChoice("current","Текущий состав",false)});
   Equal("current",model.ActiveId,"virtual current remains factual without initializing store");
   Equal(false,model.CanDelete("current"),"virtual active cannot be deleted");
   model.SetSnapshot(true,"A",new[]{new GameProfileChoice("A","Current",true),new GameProfileChoice("B","Other",false)},true);
   Equal(true,model.RecoveryPending,"interrupted transition requires recovery");
   Equal(false,model.CanDelete("B"),"recovery disables deletion");
   Equal(false,model.BeginSwitch("B"),"recovery disables another switch");
   model.SetSnapshot(true,"A",model.Profiles,false);
   Equal(false,model.RecoveryPending,"recovered snapshot clears guard");
   Equal(true,model.BeginSwitch("B"),"recovered snapshot permits switching");
  }
 }
}
'@
Add-Type -TypeDefinition ($source+"`n"+$tests) -CompilerOptions '/nullable:enable'
[ProfileUiTests.Checks]::Run()
'PASS: profile actual/pending/failure/success, delete guards, unavailable, virtual current.'

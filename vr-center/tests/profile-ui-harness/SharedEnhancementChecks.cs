using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.LogicalTree;
using Avalonia.Threading;
using Avalonia.VisualTree;
using System.Collections;
using System.Diagnostics;
using System.Reflection;
using System.Text.Json;
using System.Text.Json.Nodes;
using VRPorts;

static class SharedEnhancementChecks {
    public static void Run(string powershell) {
        var fixture=Path.Combine(Path.GetTempPath(),"VRPorts-shared-enhancement-"+Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(fixture);
        File.WriteAllText(Path.Combine(fixture,"config.json"),JsonSerializer.Serialize(new {game=Path.Combine(fixture,"game"),data=Path.Combine(fixture,"data"),pwsh=powershell,gameProfilesEnabled=false}));
        File.WriteAllText(Path.Combine(fixture,"Backend.ps1"),"""
param([string]$Request,[string]$Result,[string]$ConfigPath)
$r=Get-Content $Request -Raw|ConvertFrom-Json
if($r.action -eq 'SetEnhancementPackage'){$r|ConvertTo-Json|Set-Content (Join-Path $PSScriptRoot 'action.json')}
elseif($r.action -ne 'Status'){throw 'Unexpected fixture action'}
@{ok=$true;action=$r.action;output='fixture';state=@{
 versions=@(@{id='flat';kind='flat';family='Плоская';label='Flat';available=$true},@{id='astra';kind='native';family='Astra HD';label='Astra';available=$true})
 receipt=@{version='astra';kind='native'};exePresent=$true;libraries=@();components=@();modsEnabled=$true;recommendedSettings=$false
 gameProfiles=@{enabled=$false;available=$false;profiles=@();recoveryRequired=@()};collection=@{kind='vortex';profileName='Fixture'}
 testSaves=@{enabled=$false;sets=@();selectedIds=@()};redmod=@{enabled=$false;installed=$true;available=$true}
 enhancementPackage=@{scope='game-shared';enabled=$true;compatible=$true;available=$true;status='Общий пакет включён';sourceLabel='Common fixture'}
}}|ConvertTo-Json -Depth 12|Set-Content $Result
""");
        var root=typeof(MainWindow).Assembly.GetType("VRPorts.Program")!.GetProperty("Root",BindingFlags.Static|BindingFlags.Public)!;
        var prior=root.GetValue(null);MainWindow? window=null;
        try {
            root.SetValue(null,fixture);window=new MainWindow();window.Show();
            const BindingFlags flags=BindingFlags.Instance|BindingFlags.NonPublic;
            object? Field(string name)=>typeof(MainWindow).GetField(name,flags)!.GetValue(window);
            object? Call(string name,params object[] args)=>typeof(MainWindow).GetMethod(name,flags)!.Invoke(window,args);
            void Settle(){var t=Stopwatch.StartNew();while(t.ElapsedMilliseconds<20000){Dispatcher.UIThread.RunJobs();window.UpdateLayout();Thread.Sleep(10);if(!(bool)Field("busy")!&&t.ElapsedMilliseconds>250)return;}throw new Exception("Fixture UI timeout");}
            Settle();var state=(JsonObject)Field("state")!;var package=(JsonObject)state["enhancementPackage"]!;
            object Port(string id)=>((IEnumerable)Field("ports")!).Cast<object>().Single(p=>p.GetType().GetProperty("Id")!.GetValue(p)?.ToString()==id);
            void Select(string id)=>typeof(MainWindow).GetField("selected",flags)!.SetValue(window,Port(id));
            FeatureToggleButton Toggle(bool home=false)=>(FeatureToggleButton)Call("CreateEnhancementPackageToggle",home)!;
            int checks=0;void Check(bool value,string why){if(!value)throw new Exception(why);checks++;}
            Select("astra");Check(Toggle().IsEnabled,"RED: shared package blocked for selected native Astra");
            Check(Toggle(true).IsEnabled,"Shared package blocked on actual Astra current-launch");
            var card=(Control)Call("EnhancementPackageCard")!;
            Check(card.GetLogicalDescendants().OfType<TextBlock>().Any(x=>x.Name=="EnhancementPackageStatus"&&x.Text=="Общий пакет включён"),"Shared native card hides backend status");
            Check(!ToolTip.GetTip(Toggle())!.ToString()!.Contains("плоской"),"Shared tooltip still claims flat-only");
            package.Remove("scope");Select("flat");Check(Toggle().IsEnabled,"Legacy flat selected rejected");
            Check(!Toggle(true).IsEnabled,"Legacy actual native incorrectly follows selected flat");
            Select("astra");Check(!Toggle().IsEnabled,"Legacy native incorrectly enabled");
            state["receipt"]!["version"]="flat";Check(Toggle(true).IsEnabled,"Legacy actual flat incorrectly follows selected native");state["receipt"]!["version"]="astra";
            package["scope"]="game-shared";
            foreach(var key in new[]{"available","compatible"}){package[key]=false;Check(!Toggle().IsEnabled,"Shared ignores "+key);package[key]=true;}
            foreach(var key in new[]{"gameRunning","vortexRunning","mo2Running","modOrganizerRunning"}){state[key]=true;Check(!Toggle().IsEnabled,"Shared ignores busy "+key);state[key]=false;}
            foreach(var key in new[]{"busy","loading"}){typeof(MainWindow).GetField(key,flags)!.SetValue(window,true);Check(!Toggle().IsEnabled,"Shared ignores "+key);typeof(MainWindow).GetField(key,flags)!.SetValue(window,false);}
            var actionable=Toggle();actionable.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));Settle();
            var action=JsonNode.Parse(File.ReadAllText(Path.Combine(fixture,"action.json")))!;
            Check(action["action"]!.GetValue<string>()=="SetEnhancementPackage"&&!action["enabled"]!.GetValue<bool>(),"Toggle did not use existing backend action/target");
            Console.WriteLine($"PASS {checks} actual headless UI shared/legacy/current/selected/busy/backend-action checks");
        } finally {window?.Close();root.SetValue(null,prior);}
    }
}

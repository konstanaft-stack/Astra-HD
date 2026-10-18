using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Threading;
using Avalonia.VisualTree;
using System.Collections;
using System.Diagnostics;
using System.Reflection;
using System.Text.Json;
using System.Text.Json.Nodes;
using VRPorts;

static class CurrentLaunchChecks
{
    public static void Run(string powershell)
    {
        string fixture = Path.Combine(Path.GetTempPath(), "current-launch-ui-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(fixture);
        File.WriteAllText(Path.Combine(fixture,"config.json"), JsonSerializer.Serialize(new { game=Path.Combine(fixture,"game"),data=Path.Combine(fixture,"data"),pwsh=powershell,gameProfilesEnabled=false }));
        File.WriteAllText(Path.Combine(fixture,"Backend.ps1"), """
param([string]$Request,[string]$Result,[string]$ConfigPath)
$r=Get-Content $Request -Raw|ConvertFrom-Json
$persist=Join-Path $PSScriptRoot 'switches.json'
$toggles=if(Test-Path $persist){Get-Content $persist -Raw|ConvertFrom-Json -AsHashtable}else{@{redmod=$true;nr=$true}}
if($r.action -eq 'SetRedmodEnabled'){$toggles.redmod=[bool]$r.enabled}
elseif($r.action -eq 'SetEnhancementPackage'){$toggles.nr=[bool]$r.enabled}
elseif($r.action -ne 'Status'){throw 'Unexpected fixture action'}
$toggles|ConvertTo-Json|Set-Content $persist
@{ok=$true;action=$r.action;output='fixture';state=@{
 versions=@(@{id='flat';kind='flat';family='Плоская игра';label='Плоская игра';available=$true},@{id='saved-flat';kind='flat';family='Плоская игра';label='Сохранённая плоская сборка';available=$true},@{id='astra';kind='native';family='Astra HD';label='Astra';available=$true})
 receipt=$null;exePresent=$true;libraries=@();components=@();modsEnabled=$true;recommendedSettings=$false
 gameProfiles=@{enabled=$false;available=$false;profiles=@();recoveryRequired=@()};collection=@{kind='vortex';profileName='Super AI'}
 testSaves=@{enabled=$false;sets=@();selectedIds=@()};redmod=@{enabled=$toggles.redmod;installed=$true;available=$true}
 enhancementPackage=@{enabled=$toggles.nr;compatible=$true;available=$true;status='NR + MFG подключён через центр';sourceLabel='OptiScaler 10 + NR + MFG'}
}}|ConvertTo-Json -Depth 12|Set-Content $Result
""");
        var program=typeof(MainWindow).Assembly.GetType("VRPorts.Program")!;
        var root=program.GetProperty("Root",BindingFlags.Static|BindingFlags.Public)!;
        var previous=root.GetValue(null); MainWindow? window=null;
        try {
            root.SetValue(null,fixture);window=new MainWindow();window.Show();
            var flags=BindingFlags.Instance|BindingFlags.NonPublic;
            object? Field(string name)=>typeof(MainWindow).GetField(name,flags)!.GetValue(window);
            void Call(string name,params object[] values)=>typeof(MainWindow).GetMethod(name,flags)!.Invoke(window,values);
            void Settle(){var watch=Stopwatch.StartNew();do{Dispatcher.UIThread.RunJobs();window.UpdateLayout();Thread.Sleep(15);if(!(bool)Field("busy")!&&watch.ElapsedMilliseconds>250)return;}while(watch.ElapsedMilliseconds<20000);throw new Exception("UI did not settle");}
            T Named<T>(string name) where T:Control=>window.GetVisualDescendants().OfType<T>().Single(c=>c.Name==name);
            object Port(string id)=>((IEnumerable)Field("ports")!).Cast<object>().Single(p=>p.GetType().GetProperty("Id")!.GetValue(p)?.ToString()==id);
            void Click(Button b)=>b.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
            Settle();
            if(!Named<FeatureToggleButton>("HomeEnhancementToggle").IsEnabled || !Named<FeatureToggleButton>("HomeEnhancementToggle").Classes.Contains("On"))throw new Exception("Installed flat package not shown On on home page");
            Click(Named<FeatureToggleButton>("HomeRedmodToggle"));Settle();
            if(Named<FeatureToggleButton>("HomeRedmodToggle").Classes.Contains("On"))throw new Exception("Home REDmod toggle did not persist Off");
            Call("SelectPort",Port("saved-flat"));Call("OpenManage");Settle();
            if(!Named<FeatureToggleButton>("EnhancementPackageToggle").IsEnabled)throw new Exception("Saved flat ID wrongly treated as VR");
            typeof(MainWindow).GetField("manage",flags)!.SetValue(window,false);
            Call("SelectPort",Port("astra"));Call("BuildLibrary");Settle();
            Click(Named<Button>("CurrentLaunchSettings"));Settle();
            if(Field("selected")!.GetType().GetProperty("Id")!.GetValue(Field("selected"))?.ToString()!="flat" || !(bool)Field("advanced")!)throw new Exception("Current launch did not open active flat advanced settings");
            Console.WriteLine("PASS: home REDmod/NR state; saved flat compatibility; current-launch click opens actual mode in advanced settings");
        } finally { window?.Close();root.SetValue(null,previous); }
    }
}

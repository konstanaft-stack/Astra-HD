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

static class TestSaveUiChecks
{
    public static void Run(string powershell)
    {
        var fixture = Path.Combine(Path.GetTempPath(), "vr-test-save-ui-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(fixture);
        File.WriteAllText(Path.Combine(fixture, "config.json"), JsonSerializer.Serialize(new {
            data = Path.Combine(fixture, "data"), game = Path.Combine(fixture, "game"), pwsh = powershell, gameProfilesEnabled = false
        }));
        File.WriteAllText(Path.Combine(fixture, "Backend.ps1"), """
param([string]$Request,[string]$Result,[string]$ConfigPath)
$ErrorActionPreference='Stop'
$req=Get-Content -LiteralPath $Request -Raw | ConvertFrom-Json -AsHashtable
$saved=Join-Path $PSScriptRoot 'fixture-state.json'
$saves=if(Test-Path -LiteralPath $saved){Get-Content -LiteralPath $saved -Raw | ConvertFrom-Json -AsHashtable}else{@{
 enabled=$false;selectedIds=@('set-a');installedCount=0;libraryRoot=(Join-Path $PSScriptRoot 'library');sets=@(
  @{id='set-a';label='Сюжет';saveCount=9;selected=$true},@{id='set-b';label='Проверка города';saveCount=1;selected=$false}
 )
}}
switch($req.action){
 'Status' {}
 'SetTestSaves' {$saves.enabled=[bool]$req.enabled}
 'SetTestSaveSelection' {$saves.selectedIds=@($req.ids)}
 default {throw ('Unexpected UI fixture action: '+$req.action)}
}
foreach($set in $saves.sets){$set.selected=$set.id -in $saves.selectedIds}
$saves.installedCount=if($saves.enabled){[int](($saves.sets|Where-Object selected|Measure-Object saveCount -Sum).Sum)}else{0}
$saves|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $saved -Encoding utf8
$answer=@{ok=$true;action=$req.action;output='Копии выбранных сохранений в текущем профиле; исходники сохранены.';state=@{
 versions=@(@{id='flat';label='Плоская игра';family='Плоская игра';kind='flat';available=$true;installed=$true});receipt=@{version='flat';kind='flat'}
 gameProfiles=@{enabled=$false;available=$false;initialized=$false;activeId='current';profiles=@();recoveryRequired=@()}
 components=@();libraries=@();collection=@{kind='direct'};usbRecovery=@{enabled=$false};exePresent=$true;gameRunning=$false;vortexRunning=$false;testSaves=$saves
}}
$answer|ConvertTo-Json -Depth 18|Set-Content -LiteralPath $Result -Encoding utf8
""");
        if (File.Exists(Path.Combine(AppContext.BaseDirectory, "Backend.ps1")))
            throw new Exception("Synthetic save UI test must run without a packaged Backend.ps1 beside the harness.");
        var rootProperty = typeof(MainWindow).Assembly.GetType("VRPorts.Program")!.GetProperty("Root", BindingFlags.Public | BindingFlags.Static)!;
        var previousRoot = rootProperty.GetValue(null);
        MainWindow? window = null;
        try
        {
            rootProperty.SetValue(null, fixture);
            window = new MainWindow(); window.Show();
            var busy = typeof(MainWindow).GetField("busy", BindingFlags.Instance | BindingFlags.NonPublic)!;
            void Settle()
            {
                var watch = Stopwatch.StartNew();
                do
                {
                    Dispatcher.UIThread.RunJobs(); window.UpdateLayout(); Thread.Sleep(15);
                    if (!(bool)busy.GetValue(window)! && watch.ElapsedMilliseconds > 200) return;
                } while (watch.Elapsed < TimeSpan.FromSeconds(20));
                throw new Exception("Save UI backend did not settle.");
            }
            T Named<T>(Window owner, string name) where T : Control => owner.GetVisualDescendants().OfType<T>().Single(control => control.Name == name);
            void Click(Button button) => button.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
            Window Modal() => ((IEnumerable)typeof(Window).GetProperty("OwnedWindows")!.GetValue(window)!).Cast<Window>().Single(owner => owner.IsVisible);
            JsonNode Actual() => JsonNode.Parse(File.ReadAllText(Path.Combine(fixture, "fixture-state.json")))!;
            Settle();
            var toggle = Named<FeatureToggleButton>(window, "TestSavesToggle");
            if (!toggle.IsEnabled || !toggle.Classes.Contains("Off")) throw new Exception("Ready selected test saves do not expose the off toggle on the home screen.");
            Click(toggle);
            if (!toggle.Classes.Contains("Off") || toggle.IsEnabled) throw new Exception("Save toggle changed actual state optimistically or stayed actionable while busy.");
            Settle();
            if (Actual()["enabled"]?.GetValue<bool>() != true || Actual()["installedCount"]?.GetValue<int>() != 9)
                throw new Exception("Test save toggle did not use the immediate backend action.");
            if (!Named<FeatureToggleButton>(window, "TestSavesToggle").Classes.Contains("On")) throw new Exception("Confirmed enabled state was not rendered.");
            Click(Named<Button>(window, "TestSaveSets")); Dispatcher.UIThread.RunJobs();
            var dialog = Modal(); dialog.UpdateLayout();
            var checks = dialog.GetVisualDescendants().OfType<CheckBox>().ToArray();
            if (checks.Length != 2 || !checks.Single(check => check.Tag?.ToString() == "set-a").Content!.ToString()!.Contains("9"))
                throw new Exception("Save set names/counts were not listed.");
            if (Named<Button>(dialog, "TestSaveSetApply").IsEnabled) throw new Exception("Unchanged selection is already dirty.");
            checks.Single(check => check.Tag?.ToString() == "set-b").IsChecked = true;
            Click(Named<Button>(dialog, "TestSaveSetApply")); Settle();
            if (dialog.IsVisible || Actual()["selectedIds"]!.AsArray().Count != 2 || Actual()["installedCount"]?.GetValue<int>() != 10)
                throw new Exception("Save selection did not apply to the current profile and close.");
            Click(Named<Button>(window, "TestSaveSets")); Dispatcher.UIThread.RunJobs();
            dialog = Modal(); dialog.UpdateLayout();
            if (!Named<Button>(dialog, "TestSaveSetAddFolder").IsEnabled) throw new Exception("Import folder action is missing.");
            dialog.GetVisualDescendants().OfType<CheckBox>().First().IsChecked = false;
            Click(Named<Button>(dialog, "TestSaveSetClose")); Dispatcher.UIThread.RunJobs();
            if (Actual()["selectedIds"]!.AsArray().Count != 2) throw new Exception("Closing an uncommitted selection changed the profile.");
            Click(Named<FeatureToggleButton>(window, "TestSavesToggle")); Settle();
            if (Actual()["enabled"]?.GetValue<bool>() != false || Actual()["installedCount"]?.GetValue<int>() != 0 || Actual()["selectedIds"]!.AsArray().Count != 2)
                throw new Exception("Disabling copies lost the saved set selection.");
            Console.WriteLine("PASS: home save toggle waits for actual backend state; modal shows counts, applies stable set IDs, closes correctly, cancels drafts and retains selection when disabled. Fixture: " + fixture);
        }
        finally
        {
            window?.Close(); rootProperty.SetValue(null, previousRoot);
        }
    }
}

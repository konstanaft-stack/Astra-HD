using Avalonia.Controls;
using Avalonia;
using Avalonia.Headless;
using Avalonia.Input;
using Avalonia.Threading;
using Avalonia.VisualTree;
using System.Diagnostics;
using System.Reflection;
using System.Text.Json;
using System.Text.Json.Nodes;
using VRPorts;

static class StartupRecoveryChecks {
 public static void Run(string powershell) {
  var temporaryRoot = Path.GetFullPath(Path.GetTempPath()).TrimEnd(Path.DirectorySeparatorChar);
  var fixture = Path.GetFullPath(Path.Combine(temporaryRoot, "vr-profile-startup-ui-" + Guid.NewGuid().ToString("N")));
  if (!fixture.StartsWith(temporaryRoot + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) throw new Exception("Fixture escaped TEMP");
  Directory.CreateDirectory(fixture);
  var rootProperty = typeof(MainWindow).Assembly.GetType("VRPorts.Program")!.GetProperty("Root", BindingFlags.Public | BindingFlags.Static)!;
  var previousRoot = rootProperty.GetValue(null);
  MainWindow? window = null;
  try {
   File.WriteAllText(Path.Combine(fixture, "config.json"), JsonSerializer.Serialize(new { data = Path.Combine(fixture, "data"), game = Path.Combine(fixture, "game"), pwsh = powershell }));
   File.WriteAllText(Path.Combine(fixture, "Backend.ps1"), """
param([string]$Request,[string]$Result)
$ErrorActionPreference='Stop'
$req=Get-Content -LiteralPath $Request -Raw | ConvertFrom-Json
$profiles=@{enabled=$true;available=$true;initialized=$true;activeId='current';profiles=@(@{id='current';name='Основной';vr=$false});recoveryRequired=@()}
$stageFile=Join-Path $PSScriptRoot 'stage.txt'
$stage=if(Test-Path $stageFile){Get-Content -LiteralPath $stageFile -Raw}else{''}
if($stage -in @('rich-A','new-flat','status-failure')){
 $profiles.profiles=@(@{id='current';name='Основной';vr=$true},@{id=('b'*32);name='Чистый B';vr=$false})
 if($stage -ne 'rich-A'){$profiles.activeId='b'*32}
}
if($req.action -eq 'SwitchGameProfile'){
 Set-Content -LiteralPath $stageFile -Value 'new-flat' -NoNewline
 $profiles.activeId='b'*32
 $answer=@{ok=$false;error='Synthetic full refresh failed after committed switch';gameProfiles=$profiles}
}elseif($req.action -eq 'Status' -and $stage -eq 'status-failure'){
 Set-Content -LiteralPath $stageFile -Value 'new-flat' -NoNewline
 $answer=@{ok=$false;error='Synthetic later Status failure';gameProfiles=$profiles}
}elseif($req.action -eq 'Status' -and -not $stage){
 $profiles.recoveryRequired=@('a'*32)
 $answer=@{ok=$false;error='Synthetic broken receipt';gameProfiles=$profiles}
}elseif($req.action -eq 'RepairGameProfiles' -or $req.action -eq 'Status' -or $req.action -eq 'DuplicateGameProfile'){
 if($req.action -eq 'DuplicateGameProfile'){$profiles.profiles+=@{id=('c'*32);name=$req.name;vr=$false};$profiles.activeId='c'*32}
 if($req.action -eq 'RepairGameProfiles'){Set-Content -LiteralPath $stageFile -Value 'ready' -NoNewline}
 $version=if($stage -eq 'rich-A'){'luke-fixture'}else{'flat'}
 $rich=$stage -eq 'rich-A'
 $libraries=@();if($rich){$libraries=@(@{file='nvngx_dlss.dll';current='OLD-A-HASH';choices=@()})}
 $answer=@{ok=$true;action=$req.action;output='Synthetic recovery complete';state=@{
  gameProfiles=$profiles;versions=@(@{id=$version;label=$version;family=$(if($rich){'Luke Ross'}else{'Плоская игра'});kind=$(if($rich){'luke'}else{'flat'});available=$true;installed=$true})
  receipt=@{version=$version;kind=$(if($rich){'luke'}else{'flat'});opti=$rich;fg=$rich;hands=$rich};libraries=$libraries;components=@();usbRecovery=@{enabled=$false}
 }}
}else{throw ('Unexpected fixture action: '+$req.action)}
$answer | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $Result -Encoding utf8
""");
   rootProperty.SetValue(null, fixture);
   window = new MainWindow();
   window.Show();
   var busy = typeof(MainWindow).GetField("busy", BindingFlags.Instance | BindingFlags.NonPublic)!;
   void Settle() {
    var timer = Stopwatch.StartNew();
    do { Dispatcher.UIThread.RunJobs(); window.UpdateLayout(); Thread.Sleep(10); }
    while ((bool)busy.GetValue(window)! && timer.Elapsed < TimeSpan.FromSeconds(20));
    Dispatcher.UIThread.RunJobs(); window.UpdateLayout();
    if ((bool)busy.GetValue(window)!) throw new Exception("Synthetic backend did not finish");
   }
   Settle();
   var repair = window.GetVisualDescendants().OfType<Button>().SingleOrDefault(b => b.Content?.ToString() == "Восстановить профиль" && b.IsVisible);
   if (repair == null || !repair.IsEnabled) throw new Exception("Startup Status error hid the recovery action");
   var picker = window.GetVisualDescendants().OfType<GameProfilePicker>().Single();
   if (picker.IsEnabled) throw new Exception("Interrupted profile allowed another switch");
   repair.RaiseEvent(new Avalonia.Interactivity.RoutedEventArgs(Button.ClickEvent));
   Settle();
   if (window.GetVisualDescendants().OfType<Button>().Any(b => b.Content?.ToString() == "Восстановить профиль" && b.IsVisible)) throw new Exception("Successful recovery did not clear its action");
   if (!window.GetVisualDescendants().OfType<GameProfilePicker>().Single().IsEnabled) throw new Exception("Successful recovery did not restore profile selection");
   var plus = window.GetVisualDescendants().OfType<Button>().Single(b => b.Content?.ToString() == "+");
   var cover = (Button)typeof(MainWindow).GetField("gameCardButton", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(window)!;
   if (plus.GetVisualAncestors().Contains(cover)) throw new Exception("Create button is nested inside navigating cover button");
   var countBefore = Directory.GetFiles(Path.Combine(fixture, "data", "requests"), "*.json").Length;
   var point = plus.TranslatePoint(new Point(plus.Bounds.Width / 2, plus.Bounds.Height / 2), window)!.Value;
   window.MouseMove(point); window.MouseDown(point, MouseButton.Left); window.MouseUp(point, MouseButton.Left);
   Dispatcher.UIThread.RunJobs(); window.UpdateLayout();
   if ((bool)typeof(MainWindow).GetField("showPorts", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(window)!) throw new Exception("Plus click entered port library");
   var ownedProperty = typeof(Window).GetProperty("OwnedWindows") ?? throw new Exception("Headless fixture cannot inspect the creation dialog");
   var dialog = ((System.Collections.IEnumerable)ownedProperty.GetValue(window)!).Cast<Window>().Single(w => w.IsVisible);
   if (dialog.GetVisualDescendants().OfType<FeatureToggleButton>().Any()) throw new Exception("Name dialog still contains VR toggle");
   var cancel = dialog.GetVisualDescendants().OfType<Button>().Single(b => b.Content?.ToString() == "Отмена");
   cancel.RaiseEvent(new Avalonia.Interactivity.RoutedEventArgs(Button.ClickEvent));
   Dispatcher.UIThread.RunJobs();
   if (Directory.GetFiles(Path.Combine(fixture, "data", "requests"), "*.json").Length != countBefore) throw new Exception("Cancel submitted a backend mutation");
   void Request(string action, JsonObject? extra = null) {
    var method = typeof(MainWindow).GetMethod("Run", BindingFlags.Instance | BindingFlags.NonPublic)!;
    var task = (System.Threading.Tasks.Task)method.Invoke(window, new object?[] { action, extra })!;
    Settle();
    if (!task.IsCompleted) throw new Exception("UI action did not settle");
    task.GetAwaiter().GetResult();
   }
   object? Field(string name) => typeof(MainWindow).GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(window);
   File.WriteAllText(Path.Combine(fixture, "stage.txt"), "rich-A");
   Request("Status");
   if (((Port?)Field("selected"))?.Id != "luke-fixture" || !(bool)Field("opti")! || !(bool)Field("fg")! || !(bool)Field("hands")!) throw new Exception("Fixture did not establish the old VR draft");
   Request("SwitchGameProfile", new JsonObject { ["profileId"] = new string('b', 32) });
   var selection = (GameProfileSelection)Field("gameProfileSelection")!;
   if (selection.ActiveId != new string('b', 32) || selection.RecoveryPending || selection.Marker(selection.ActiveId) != GameProfileMarker.Active) throw new Exception("Committed actual catalog was not retained on refresh failure");
   cover = (Button)Field("gameCardButton")!;
   var partialState = (JsonObject)Field("state")!;
   if (cover.IsEnabled || Field("selected") != null || ((System.Collections.ICollection)Field("ports")!).Count != 0 || ((System.Collections.ICollection)Field("libraries")!).Count != 0 || partialState["receipt"] != null || partialState["versions"] != null) throw new Exception("Partial profile refresh exposed the old receipt/ports/libraries");
   if ((bool)Field("opti")! || (bool)Field("fg")! || (bool)Field("hands")! || (bool)Field("verified")!) throw new Exception("Partial refresh retained old active VR flags");
   var beforeBlocked = Directory.GetFiles(Path.Combine(fixture, "data", "requests"), "*.json").Length;
   Request("ApplyConfiguration");
   if (Directory.GetFiles(Path.Combine(fixture, "data", "requests"), "*.json").Length != beforeBlocked) throw new Exception("Stale port action reached backend after partial refresh");
   var refresh = window.GetVisualDescendants().OfType<Button>().Single(b => b.Content?.ToString() == "Обновить состояние" && b.IsVisible);
   refresh.RaiseEvent(new Avalonia.Interactivity.RoutedEventArgs(Button.ClickEvent));
   Settle();
   if (((Port?)Field("selected"))?.Id != "flat" || (bool)Field("opti")! || (bool)Field("fg")! || (bool)Field("hands")! || !(bool)Field("portStateCurrent")! || !((Button)Field("gameCardButton")!).IsEnabled) throw new Exception("Successful full Status did not restore the new profile's flat state");
   File.WriteAllText(Path.Combine(fixture, "stage.txt"), "status-failure");
   Request("Status");
   if ((bool)Field("portStateCurrent")! || Field("selected") != null || ((JsonObject)Field("state")!)["receipt"] != null || ((Button)Field("gameCardButton")!).IsEnabled || selection.ActiveId != new string('b', 32)) throw new Exception("Later failed Status exposed a previously valid port snapshot");
   Request("Status");
   if (!(bool)Field("portStateCurrent")! || ((Port?)Field("selected"))?.Id != "flat") throw new Exception("Status retry did not recover after later failure");
   var duplicate = window.GetVisualDescendants().OfType<Button>().Single(b => b.Name == "DuplicateGameProfile");
   duplicate.RaiseEvent(new Avalonia.Interactivity.RoutedEventArgs(Button.ClickEvent));
   Dispatcher.UIThread.RunJobs(); window.UpdateLayout();
   var copyDialog = ((System.Collections.IEnumerable)ownedProperty.GetValue(window)!).Cast<Window>().Single(w => w.IsVisible);
   if (copyDialog.Title != "Дублировать профиль") throw new Exception("Duplicate button opened the wrong dialog");
   copyDialog.GetVisualDescendants().OfType<TextBox>().Single().Text = "VR copy";
   copyDialog.GetVisualDescendants().OfType<Button>().Single(b => b.Content?.ToString() == "Дублировать").RaiseEvent(new Avalonia.Interactivity.RoutedEventArgs(Button.ClickEvent));
   Settle();
   if (copyDialog.IsVisible || selection.ActiveId != new string('c',32)) throw new Exception("Duplicate dialog did not activate returned copy");
   var copyRequest = Directory.GetFiles(Path.Combine(fixture,"data","requests"),"*.json")
    .Select(p => JsonNode.Parse(File.ReadAllText(p))!).FirstOrDefault(n => n["action"]?.ToString() == "DuplicateGameProfile" && n["name"]?.ToString() == "VR copy");
   if (copyRequest == null) throw new Exception("Duplicate dialog did not send a named duplicate request");
   Console.WriteLine("PASS: actual duplicate button opens name dialog, submits named copy action and waits for active profile from backend.");
   Console.WriteLine("PASS: production cover plus pointer opens name-only dialog without entering ports; cancel sends no backend action.");
   Console.WriteLine("PASS: real MainWindow startup failure exposes recovery; recovery result restores game-card controls. Synthetic backend only.");
   Console.WriteLine("PASS: committed switch with failed refresh preserves actual B catalog, discards old VR state, blocks stale actions and requires full Status.");
  } finally {
   window?.Close();
   rootProperty.SetValue(null, previousRoot);
   // Keep the bounded fixture and its exact request/response evidence for review.
   Console.WriteLine("Startup UI evidence: " + fixture);
  }
 }
}

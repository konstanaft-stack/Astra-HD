using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Threading;
using Avalonia.VisualTree;
using System.Collections;
using System.Diagnostics;
using System.Reflection;
using System.Text.Json.Nodes;
using VRPorts;

static class CollectionImportChecks
{
    public static void Run(string fixture)
    {
        var rootProperty = typeof(MainWindow).Assembly.GetType("VRPorts.Program")!.GetProperty("Root", BindingFlags.Public | BindingFlags.Static)!;
        var previousRoot = rootProperty.GetValue(null);
        MainWindow? window = null;
        try
        {
            rootProperty.SetValue(null, fixture);
            window = new MainWindow(); window.Show();
            void Settle()
            {
                var watch = Stopwatch.StartNew();
                do
                {
                    Dispatcher.UIThread.RunJobs(); window.UpdateLayout(); Thread.Sleep(15);
                    bool busy = (bool)typeof(MainWindow).GetField("busy", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(window)!;
                    if (!busy && watch.ElapsedMilliseconds > 150) return;
                } while (watch.Elapsed < TimeSpan.FromSeconds(20));
                throw new Exception("Collection UI did not settle.");
            }
            Button Button(Window owner, string text) => owner.GetVisualDescendants().OfType<Button>().Single(b => b.Content?.ToString() == text);
            void Click(Button button) => button.RaiseEvent(new RoutedEventArgs(Avalonia.Controls.Button.ClickEvent));
            Settle();
            var capture=window.GetVisualDescendants().OfType<Button>().SingleOrDefault(b=>b.Content?.ToString()=="Сохранить чистую основу");
            if(capture!=null){
                Click(capture);Dispatcher.UIThread.RunJobs();
                var confirmations=(IEnumerable)typeof(Window).GetProperty("OwnedWindows")!.GetValue(window)!;
                var confirm=confirmations.Cast<Window>().Single(w=>w.IsVisible);confirm.UpdateLayout();
                Click(Button(confirm,"Да"));Settle();
                if(!File.Exists(Path.Combine(fixture,"data","profile-store","clean-baseline.json")))throw new Exception("Capture UI did not save baseline.");
                var enabledConfig=JsonNode.Parse(File.ReadAllText(Path.Combine(fixture,"config.json")))!;
                if(enabledConfig["gameProfilesEnabled"]?.GetValue<bool>()!=true)throw new Exception("Capture did not enable profiles.");
                Console.WriteLine("PASS: actual clean-base button -> explicit first-menu confirmation -> packaged capture -> full profiles enabled.");
            }
            Click(Button(window, "Коллекция…")); Dispatcher.UIThread.RunJobs();
            var owned = (IEnumerable)typeof(Window).GetProperty("OwnedWindows")!.GetValue(window)!;
            var dialog = owned.Cast<Window>().Single(w => w.IsVisible);
            Settle();
            dialog.UpdateLayout();
            dialog.GetVisualDescendants().OfType<ComboBox>().Single(control => control.Name == "CollectionSource").SelectedIndex = 1;
            Dispatcher.UIThread.RunJobs();
            dialog.GetVisualDescendants().OfType<TextBox>().Single(control => control.Name == "CollectionRoot").Text = Path.Combine(fixture, "MO2 collection");
            Click(Button(dialog, "Прочитать")); Settle(); dialog.UpdateLayout();
            var profiles = dialog.GetVisualDescendants().OfType<ComboBox>().Single(control => control.Name == "CollectionProfiles");
            if (profiles.SelectedItem?.ToString() != "Collection") throw new Exception("Profile was not populated from backend.");
            Click(Button(dialog, "Подключить")); Settle();
            var sourcePath = Path.Combine(fixture, "data", "launch-source.json");
            var source = JsonNode.Parse(File.ReadAllText(sourcePath))!;
            if (source["kind"]?.ToString() != "mo2" || dialog.IsVisible) throw new Exception("Import UI did not commit and close.");
            Click(Button(window, "Отключить")); Settle();
            source = JsonNode.Parse(File.ReadAllText(sourcePath))!;
            if (source["kind"]?.ToString() != "direct") throw new Exception("Disconnect UI did not save direct source.");
            Console.WriteLine("PASS: actual collection dialog -> packaged backend -> persisted import -> disconnect; no game or MO2 launch.");
            Click(Button(window,"Настройки игры")); Settle();
            owned=(IEnumerable)typeof(Window).GetProperty("OwnedWindows")!.GetValue(window)!;
            var graphics=owned.Cast<Window>().Single(w=>w.IsVisible);graphics.UpdateLayout();
            var fgButton=graphics.GetVisualDescendants().OfType<Button>().Single(b=>b.Name=="GameFrameGeneration");
            Click(fgButton);Dispatcher.UIThread.RunJobs();
            Click(Button(graphics,"Скорость")); Dispatcher.UIThread.RunJobs();
            var save=Button(graphics,"Сохранить");if(!save.IsEnabled)throw new Exception("Graphics preset did not stage changes.");
            Click(save);Settle();
            var config=JsonNode.Parse(File.ReadAllText(Path.Combine(fixture,"config.json")))!;
            var settings=JsonNode.Parse(File.ReadAllText(config["settingsFile"]!.ToString()))!;
            var dlss=settings["data"]!.AsArray().Single(g=>g?["group_name"]?.ToString()=="/graphics/presets")!["options"]!.AsArray().Single(o=>o?["name"]?.ToString()=="DLSS")!;
            if(dlss["value"]?.ToString()!="Performance"||dlss["index"]?.GetValue<int>()!=4)throw new Exception("Graphics UI did not persist matching DLSS value/index.");
            JsonNode GameFg(JsonNode doc)=>doc["data"]!.AsArray().Single(g=>g?["group_name"]?.ToString()=="/graphics/presets")!["options"]!.AsArray().Single(o=>o?["name"]?.ToString()=="FrameGeneration")!;
            if(GameFg(settings)["value"]?.ToString()!="DLSS" || GameFg(settings)["index"]?.GetValue<int>()!=1)throw new Exception("Game FG switch did not persist DLSS selection or preset changed it.");
            graphics.UpdateLayout();
            Click(graphics.GetVisualDescendants().OfType<Button>().Single(b=>b.Name=="GameFrameGeneration"));Dispatcher.UIThread.RunJobs();
            Click(Button(graphics,"Сохранить"));Settle();
            settings=JsonNode.Parse(File.ReadAllText(config["settingsFile"]!.ToString()))!;
            if(GameFg(settings)["value"]?.ToString()!="Off" || GameFg(settings)["index"]?.GetValue<int>()!=0)throw new Exception("Game FG switch did not persist Off selection.");
            Click(Button(graphics,"Закрыть"));Dispatcher.UIThread.RunJobs();
            Console.WriteLine("PASS: actual settings dialog -> game FG on -> quality preset preserves FG -> save -> FG off -> verified UserSettings.json; no game launch.");
        }
        finally
        {
            window?.Close(); rootProperty.SetValue(null, previousRoot);
        }
    }
}

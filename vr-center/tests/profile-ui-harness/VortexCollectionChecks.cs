using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Threading;
using Avalonia.VisualTree;
using System.Collections;
using System.Diagnostics;
using System.Reflection;
using System.Text.Json.Nodes;
using VRPorts;

// Invoke only with an isolated packaged-backend fixture containing the Vortex
// profile "SuperCyberpunkAI"; never point the fixture at the installed center.
static class VortexCollectionChecks
{
    public static void Run(string fixture)
    {
        fixture = Path.GetFullPath(fixture);
        var config = JsonNode.Parse(File.ReadAllText(Path.Combine(fixture, "config.json")))!;
        foreach (var key in new[] { "data", "game", "vortexData" })
        {
            var path = Path.GetFullPath(config[key]?.ToString() ?? throw new Exception("Isolated fixture config is incomplete."));
            if (!path.StartsWith(fixture.TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                throw new Exception("Vortex UI fixture must own its data, game and Vortex source paths.");
        }
        var rootProperty = typeof(MainWindow).Assembly.GetType("VRPorts.Program")!
            .GetProperty("Root", BindingFlags.Public | BindingFlags.Static)!;
        var previousRoot = rootProperty.GetValue(null);
        MainWindow? window = null;
        try
        {
            rootProperty.SetValue(null, fixture);
            window = new MainWindow();
            window.Show();
            var busyField = typeof(MainWindow).GetField("busy", BindingFlags.Instance | BindingFlags.NonPublic)!;
            void Settle()
            {
                var watch = Stopwatch.StartNew();
                do
                {
                    Dispatcher.UIThread.RunJobs();
                    window.UpdateLayout();
                    Thread.Sleep(15);
                    if (!(bool)busyField.GetValue(window)! && watch.ElapsedMilliseconds > 200) return;
                } while (watch.Elapsed < TimeSpan.FromSeconds(30));
                throw new Exception("Vortex collection UI did not settle.");
            }
            void Click(Button button) => button.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
            T Named<T>(Window owner, string name) where T : Control => owner.GetVisualDescendants()
                .OfType<T>().Single(control => control.Name == name);
            Settle();
            Click(window.GetVisualDescendants().OfType<Button>().Single(button => button.Content?.ToString() == "Коллекция…"));
            Dispatcher.UIThread.RunJobs();
            var dialog = ((IEnumerable)typeof(Window).GetProperty("OwnedWindows")!.GetValue(window)!)
                .Cast<Window>().Single(owner => owner.IsVisible);
            Settle();
            dialog.UpdateLayout();
            var source = Named<ComboBox>(dialog, "CollectionSource");
            var root = Named<TextBox>(dialog, "CollectionRoot");
            var profiles = Named<ComboBox>(dialog, "CollectionProfiles");
            var add = Named<Button>(dialog, "CollectionImport");
            if (source.SelectedItem?.ToString() != "Vortex") throw new Exception("Configured Vortex was not selected by default.");
            if (root.GetVisualAncestors().OfType<Control>().All(control => control.IsVisible))
                throw new Exception("Vortex unnecessarily requires a collection folder.");
            var choices = profiles.Items.OfType<ComboBoxItem>().ToArray();
            var target = choices.Single(item => item.Content?.ToString()?.StartsWith("SuperCyberpunkAI ·", StringComparison.Ordinal) == true);
            var targetData = (JsonNode)target.Tag!;
            var profileId = targetData["id"]!.ToString();
            if (profileId == "SuperCyberpunkAI" || !target.IsEnabled) throw new Exception("Fixture must exercise a stable profile ID distinct from its available display name.");
            foreach (var item in choices)
            {
                var data = (JsonNode)item.Tag!;
                if (data["available"]?.GetValue<bool>() == false && item.IsEnabled)
                    throw new Exception("Unavailable Vortex profile is selectable.");
            }
            profiles.SelectedItem = target;
            dialog.UpdateLayout();
            if (!add.IsEnabled) throw new Exception("Available Vortex profile did not enable import.");
            Click(add);
            Settle();
            if (dialog.IsVisible) throw new Exception("Successful Vortex import did not close its dialog.");
            var requests = Directory.GetFiles(Path.Combine(fixture, "data", "requests"), "*.json")
                .Where(path => !path.EndsWith("-result.json", StringComparison.Ordinal))
                .Select(path => JsonNode.Parse(File.ReadAllText(path))!).ToArray();
            var inspect = requests.Last(request => request["action"]?.ToString() == "InspectCollection");
            var import = requests.Last(request => request["action"]?.ToString() == "ImportGameCollection");
            if (inspect["provider"]?.ToString() != "vortex" || inspect["root"] != null)
                throw new Exception("Vortex inspection did not use the pathless provider contract.");
            if (import["provider"]?.ToString() != "vortex" || import["profile"]?.ToString() != profileId || import["root"] != null)
                throw new Exception("Vortex import sent its display name or required a MO2 folder.");
            var actual = JsonNode.Parse(File.ReadAllText(Path.Combine(fixture, "data", "launch-source.json")))!;
            if (actual["kind"]?.ToString() != "vortex") throw new Exception("Import did not persist the Vortex collection source.");
            if (!Named<TextBlock>(window, "CollectionCurrent").Text!.Contains("SuperCyberpunkAI", StringComparison.Ordinal))
                throw new Exception("Collection card did not show the imported friendly profile name.");
            Click(window.GetVisualDescendants().OfType<Button>().Single(button => button.Content?.ToString() == "Коллекция…"));
            Settle();
            dialog = ((IEnumerable)typeof(Window).GetProperty("OwnedWindows")!.GetValue(window)!)
                .Cast<Window>().Single(owner => owner.IsVisible);
            dialog.UpdateLayout();
            profiles = Named<ComboBox>(dialog, "CollectionProfiles");
            if (profiles.SelectedItem is not ComboBoxItem selected || ((JsonNode)selected.Tag!)["id"]?.ToString() != profileId)
                throw new Exception("Reopened collection dialog lost its imported Vortex profile selection.");
            Click(dialog.GetVisualDescendants().OfType<Button>().Single(button => button.Content?.ToString() == "Отмена"));
            Console.WriteLine("PASS: actual collection dialog discovers installed Vortex profiles, displays names, imports stable ID without a path, persists source and reselects the imported profile.");
        }
        finally
        {
            window?.Close();
            rootProperty.SetValue(null, previousRoot);
        }
    }
}

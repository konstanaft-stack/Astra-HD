using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Platform.Storage;
using System;
using System.Linq;
using System.Text.Json.Nodes;
using System.Threading.Tasks;

namespace VRPorts;
public partial class MainWindow
{
    Control BuildCollectionControls()
    {
        var source = state?["collection"];
        var kind = S(source, "kind");
        bool imported = kind is "mo2" or "vortex";
        var profileName = S(source, "profileName");
        if (profileName == "") profileName = S(source, "name");
        if (profileName == "") profileName = S(source, "profile");
        var label = Text(imported ? profileName : "Файлы игры · без виртуальной коллекции", 11, "#A5B9AC");
        label.Name = "CollectionCurrent";
        label.TextWrapping = TextWrapping.NoWrap; label.TextTrimming = TextTrimming.CharacterEllipsis;
        ToolTip.SetTip(label, imported ? "Импорт: " + S(source, "importedUtc") + "\n" + S(source?["origin"], "root") : "Обычный запуск. Физические моды Vortex и ручной установки остаются в игре.");
        Button Small(string text, Action action, string tip)
        {
            var button = Btn(text, action, "AppleSecondaryBtn", tip);
            button.MinWidth = 0; button.MinHeight = 0; button.Padding = new Thickness(8, 5);
            button.HorizontalAlignment = HorizontalAlignment.Left; button.IsEnabled = !busy && !loading;
            return button;
        }
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 5 };
        actions.Children.Add(Small("Коллекция…", () => _ = ImportCollectionDialog(), "Выбрать установленную коллекцию и профиль Vortex или Mod Organizer. Менеджер запускать не нужно."));
        if (imported)
        {
            actions.Children.Add(Small("↻", () => _ = Run("RefreshGameCollection"), "Обновить состав из выбранного профиля коллекции. Текущие настройки игры сохраняются."));
            actions.Children.Add(Small("Отключить", () => _ = Run("UseDirectCollection"), "Отключить виртуальную коллекцию без удаления её файлов."));
        }
        return new StackPanel { Spacing = 5, Children = { label, actions } };
    }

    async Task ImportCollectionDialog()
    {
        if (busy) return;
        var dialog = new Window { Name = "CollectionDialog", Title = "Подключить коллекцию", Width = 500, Height = 420,
            WindowStartupLocation = WindowStartupLocation.CenterOwner, Background = Brush("#101C15") };
        var imported = state?["collection"];
        var importedKind = S(imported, "kind");
        var source = new ComboBox { Name = "CollectionSource", ItemsSource = new[] { "Vortex", "Mod Organizer" },
            SelectedIndex = importedKind == "mo2" ? 1 : importedKind == "vortex" || S(cfg, "vortexData") != "" ? 0 : 1,
            HorizontalAlignment = HorizontalAlignment.Stretch };
        var root = new TextBox { Name = "CollectionRoot", Text = importedKind == "mo2" ? S(imported?["origin"], "root") : S(cfg["mo2"], "root"),
            PlaceholderText = "Папка коллекции с ModOrganizer.ini" };
        var profiles = new ComboBox { Name = "CollectionProfiles", HorizontalAlignment = HorizontalAlignment.Stretch, PlaceholderText = "Профиль коллекции" };
        var note = Text("", 12, "#A5B9AC");
        note.Name = "CollectionNote";
        var redmod = new CheckBox { Name = "CollectionRedmod", Content = "Запуск с REDmod", IsChecked = (imported?["arguments"] as JsonArray)?.Any(a => a?.ToString() == "-modded") == true };
        ToolTip.SetTip(redmod, "Соответствует аргументу -modded. Нужен для варианта запуска с REDmod.");
        bool reading = false;
        bool VortexSelected() => source.SelectedIndex == 0;
        Button browse = null!, read = null!, add = null!;
        browse = Btn("Обзор…", async () => {
            var picked = await dialog.StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions { Title = "Папка коллекции", AllowMultiple = false });
            if (picked.Count > 0) { root.Text = picked[0].TryGetLocalPath(); await Inspect(); }
        });
        browse.Name = "CollectionBrowse";
        read = Btn("Прочитать", async () => await Inspect());
        read.Name = "CollectionRead";
        var cancel = Btn("Отмена", dialog.Close);
        add = Btn("Подключить", async () => {
            if (busy || reading) return;
            JsonObject request;
            if (VortexSelected())
            {
                if (profiles.SelectedItem is not ComboBoxItem item || item.Tag is not JsonNode profile || !B(profile, "available")) return;
                request = new JsonObject { ["provider"] = "vortex", ["profile"] = S(profile, "id"), ["redmod"] = redmod.IsChecked == true };
            }
            else
            {
                if (profiles.SelectedItem is not string profile || string.IsNullOrWhiteSpace(root.Text)) return;
                request = new JsonObject { ["provider"] = "mo2", ["root"] = root.Text.Trim(), ["profile"] = profile, ["redmod"] = redmod.IsChecked == true };
            }
            reading = true;
            UpdateAvailability();
            try
            {
                await Run("ImportGameCollection", request);
                if (!error) dialog.Close(); else note.Text = result;
            }
            finally { reading = false; UpdateAvailability(); }
        }, "ApplePrimaryBtn");
        add.Name = "CollectionImport";
        void UpdateAvailability()
        {
            bool available = !reading && !busy;
            source.IsEnabled = root.IsEnabled = browse.IsEnabled = redmod.IsEnabled = available;
            read.IsEnabled = available && (VortexSelected() || !string.IsNullOrWhiteSpace(root.Text));
            profiles.IsEnabled = available;
            add.IsEnabled = available && (VortexSelected()
                ? profiles.SelectedItem is ComboBoxItem { IsEnabled: true }
                : profiles.SelectedItem is string && !string.IsNullOrWhiteSpace(root.Text));
        }
        async Task Inspect()
        {
            if (reading || busy || (!VortexSelected() && string.IsNullOrWhiteSpace(root.Text))) return;
            reading = true;
            profiles.ItemsSource = null;
            note.Text = "Читаю установленные профили…";
            UpdateAvailability();
            try
            {
                await Run("InspectCollection", VortexSelected()
                    ? new JsonObject { ["provider"] = "vortex" }
                    : new JsonObject { ["provider"] = "mo2", ["root"] = root.Text!.Trim() });
                if (error) { note.Text = result; return; }
                var catalog = state?["collectionCatalog"];
                var entries = catalog?["profiles"] as JsonArray;
                if (VortexSelected())
                {
                    var items = (entries ?? []).OfType<JsonObject>().Select(profile => {
                        var name = S(profile, "name");
                        if (name == "") name = S(profile, "id");
                        var item = new ComboBoxItem { Content = name + " · " + S(profile, "enabledCount") + " модов",
                            Tag = profile.DeepClone(), IsEnabled = B(profile, "available") };
                        ToolTip.SetTip(item, item.IsEnabled ? name : S(profile, "reason"));
                        return item;
                    }).ToArray();
                    var importedId = S(imported?["origin"], "profileId");
                    if (importedId == "") importedId = S(imported, "profileId");
                    if (importedId == "") importedId = S(imported, "profile");
                    var activeId = S(catalog, "activeProfileId");
                    profiles.ItemsSource = items;
                    profiles.SelectedItem = items.FirstOrDefault(item => item.IsEnabled && importedKind == "vortex" && S(item.Tag as JsonNode, "id") == importedId)
                        ?? items.FirstOrDefault(item => item.IsEnabled && (S(item.Tag as JsonNode, "id") == activeId || B(item.Tag as JsonNode, "active")))
                        ?? items.FirstOrDefault(item => item.IsEnabled);
                    if (profiles.SelectedItem is ComboBoxItem selectedProfile && S(selectedProfile.Tag as JsonNode, "id") != importedId)
                        redmod.IsChecked = B(selectedProfile.Tag as JsonNode, "requiresRedmod");
                    note.Text = items.Any(item => item.IsEnabled) ? "Выбери профиль. Коллекция будет запускаться из VR-центра." :
                        items.Length > 0 ? S(items[0].Tag as JsonNode, "reason") : "Установленные профили Cyberpunk в Vortex не найдены.";
                    if (string.IsNullOrWhiteSpace(note.Text)) note.Text = "Установленные профили пока недоступны для подключения.";
                }
                else
                {
                    var names = (entries ?? []).Select(n => n?.ToString() ?? "").ToArray();
                    profiles.ItemsSource = names;
                    profiles.SelectedItem = names.FirstOrDefault(name => importedKind == "mo2" && name == S(imported, "profile")) ?? names.FirstOrDefault();
                    note.Text = names.Length > 0 ? "Выбери профиль и подключи коллекцию." : "В этой папке не найдено профилей с modlist.txt.";
                }
            }
            finally { reading = false; UpdateAvailability(); }
        }
        var folderControls = new StackPanel { Spacing = 8, Children = {
            root, new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6, Children = { browse } }
        } };
        void SetSource()
        {
            folderControls.IsVisible = !VortexSelected();
            profiles.ItemsSource = null;
            note.Text = VortexSelected() ? "Установленные профили Vortex. Менеджер запускать не нужно." : "Выбери папку уже установленной коллекции.";
            UpdateAvailability();
        }
        source.SelectionChanged += async (_, _) => { SetSource(); if (VortexSelected()) await Inspect(); };
        root.TextChanged += (_, _) => { if (!reading) { profiles.ItemsSource = null; UpdateAvailability(); } };
        profiles.SelectionChanged += (_, _) => { if (VortexSelected() && profiles.SelectedItem is ComboBoxItem item) redmod.IsChecked = S(item.Tag as JsonNode, "id") == S(imported?["origin"], "profileId") ? (imported?["arguments"] as JsonArray)?.Any(a => a?.ToString() == "-modded") == true : B(item.Tag as JsonNode, "requiresRedmod"); UpdateAvailability(); };
        foreach (var button in new[] { browse, read, cancel, add }) { button.MinWidth = 0; button.Padding = new Thickness(10, 6); }
        dialog.Closing += (_, e) => { if (busy) e.Cancel = true; };
        dialog.Content = new StackPanel { Margin = new Thickness(20), Spacing = 12, Children = {
            source, folderControls, read,
            profiles, redmod, note, new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Children = { cancel, add } }
        } };
        SetSource();
        dialog.Opened += async (_, _) => { if (VortexSelected()) await Inspect(); };
        await dialog.ShowDialog(this);
    }
}

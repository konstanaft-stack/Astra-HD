using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Platform.Storage;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json.Nodes;
using System.Threading.Tasks;

namespace VRPorts;

public partial class MainWindow
{
    FeatureToggleButton? testSavesToggle;
    Button? testSaveSetButton;
    TextBlock? testSavesStatus;

    bool TestSavesBlocked => busy || loading || !portStateCurrent || B(state, "gameRunning")
        || gameProfileSelection.RecoveryPending || gameProfileSelection.PendingId != null;

    HashSet<string> SelectedTestSaveIds() => new((state?["testSaves"]?["selectedIds"] as JsonArray ?? [])
        .Select(id => id?.ToString() ?? "").Where(id => id != ""), StringComparer.Ordinal);

    Control BuildTestSaveControls()
    {
        var toggle = new FeatureToggleButton(new FeatureToggleBinding(
            () => new FeatureToggleState("Тестовые сохранения", null, B(state?["testSaves"], "enabled"),
                B(state?["testSaves"], "enabled"), state?["testSaves"] != null && (SelectedTestSaveIds().Count > 0 || B(state?["testSaves"], "enabled"))),
            enabled => _ = Run("SetTestSaves", new JsonObject { ["enabled"] = enabled })),
            () => TestSavesBlocked,
            "Копии выбранных сохранений в текущем профиле; исходники сохранены.",
            "Выбери набор тестовых сохранений кнопкой «Набор…».",
            () => B(state, "gameRunning") ? "Изменение доступно после выхода из игры." : "Дождись завершения текущей операции.")
        {
            Name = "TestSavesToggle", MinHeight = 0, MinWidth = 0, Padding = new Thickness(9, 6)
        };
        testSavesToggle = toggle;
        testSaveSetButton = Btn("Набор…", () => _ = TestSaveSetsDialog(), "AppleSecondaryBtn", "Выбрать сохранения из общей библиотеки или добавить папку.");
        testSaveSetButton.Name = "TestSaveSets";
        testSaveSetButton.MinHeight = 0;
        testSaveSetButton.MinWidth = 0;
        testSaveSetButton.Padding = new Thickness(9, 6);
        testSavesStatus = Text("", 11, "#91A9B8");
        testSavesStatus.Name = "TestSavesStatus";
        var controls = new StackPanel { Spacing = 5, Children = {
            new StackPanel { Orientation = Orientation.Horizontal, Spacing = 7, Children = { BuildHomeRedmodToggle(), BuildHomeEnhancementToggle(), toggle, testSaveSetButton } },
            testSavesStatus
        } };
        RefreshTestSaveControls();
        return controls;
    }

    void RefreshTestSaveControls()
    {
        testSavesToggle?.Refresh();
        if (testSaveSetButton != null) testSaveSetButton.IsEnabled = state?["testSaves"] != null && !TestSavesBlocked;
        if (testSavesStatus != null)
        {
            var saves = state?["testSaves"];
            testSavesStatus.Text = saves == null ? "Читаю наборы сохранений…" : B(saves, "enabled")
                ? "В текущем профиле: " + S(saves, "installedCount") + " сохранений. Исходники сохранены."
                : "Копии выбранных сохранений в текущем профиле; исходники сохранены.";
        }
    }

    async Task TestSaveSetsDialog()
    {
        if (TestSavesBlocked || state?["testSaves"] == null) return;
        var dialog = new Window { Name = "TestSaveDialog", Title = "Тестовые сохранения", Width = 480, Height = 390,
            Background = Brush("#101C15"), WindowStartupLocation = WindowStartupLocation.CenterOwner };
        var sets = new StackPanel { Name = "TestSaveSetList", Spacing = 9 };
        var note = Text("Копии выбранных сохранений в текущем профиле; исходники сохранены.", 12, "#A5B9AC");
        note.Name = "TestSaveSetNote";
        var choices = new Dictionary<string, CheckBox>(StringComparer.Ordinal);
        bool working = false;
        Button apply = null!, add = null!, cancel = null!;

        HashSet<string> CheckedIds() => new(choices.Where(pair => pair.Value.IsChecked == true).Select(pair => pair.Key), StringComparer.Ordinal);
        void RefreshActions()
        {
            bool available = !working && !TestSavesBlocked;
            sets.IsEnabled = available;
            add.IsEnabled = available;
            cancel.IsEnabled = !working && !busy;
            apply.IsEnabled = available && !CheckedIds().SetEquals(SelectedTestSaveIds());
        }
        void Populate(HashSet<string>? draft = null)
        {
            choices.Clear();
            sets.Children.Clear();
            var selected = draft ?? SelectedTestSaveIds();
            foreach (var entry in state?["testSaves"]?["sets"] as JsonArray ?? [])
            {
                var id = S(entry, "id");
                if (id == "") continue;
                var choice = new CheckBox { Content = S(entry, "label") + " · сохранений: " + S(entry, "saveCount"),
                    Tag = id, IsChecked = selected.Contains(id) || (draft == null && B(entry, "selected")) };
                choice.IsCheckedChanged += (_, _) => RefreshActions();
                choices.Add(id, choice);
                sets.Children.Add(choice);
            }
            if (choices.Count == 0) sets.Children.Add(Text("Наборов пока нет. Добавь папку с сохранениями.", 12, "#A5B9AC"));
            RefreshActions();
        }
        apply = Btn("Применить", async () => {
            if (working || TestSavesBlocked) return;
            var ids = new JsonArray(CheckedIds().Select(id => (JsonNode?)JsonValue.Create(id)).ToArray());
            working = true; RefreshActions();
            try
            {
                await Run("SetTestSaveSelection", new JsonObject { ["ids"] = ids });
                if (!error) { working = false; dialog.Close(); } else note.Text = result;
            }
            finally { working = false; RefreshActions(); }
        }, "ApplePrimaryBtn");
        apply.Name = "TestSaveSetApply";
        add = Btn("Добавить папку…", async () => {
            if (working || TestSavesBlocked) return;
            var picked = await dialog.StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions {
                Title = "Папка с сохранениями", AllowMultiple = false
            });
            if (picked.Count == 0 || picked[0].TryGetLocalPath() is not string path) return;
            var draft = CheckedIds();
            var existingIds = choices.Keys.ToHashSet(StringComparer.Ordinal);
            working = true; RefreshActions();
            try
            {
                await Run("ImportTestSaveSet", new JsonObject { ["path"] = path });
                if (!error)
                {
                    // Preserve uncommitted checkbox choices while accepting the
                    // backend's selection of any newly imported sets.
                    draft.UnionWith(SelectedTestSaveIds().Where(id => !existingIds.Contains(id)));
                    Populate(draft);
                    note.Text = "Папка добавлена в библиотеку. Выбери наборы для текущего профиля.";
                }
                else note.Text = result;
            }
            finally { working = false; RefreshActions(); }
        });
        add.Name = "TestSaveSetAddFolder";
        cancel = Btn("Закрыть", dialog.Close);
        cancel.Name = "TestSaveSetClose";
        foreach (var button in new[] { apply, add, cancel }) { button.MinWidth = 0; button.MinHeight = 0; button.Padding = new Thickness(10, 6); }
        dialog.Closing += (_, e) => { if (working || busy) e.Cancel = true; };
        var content = new Grid { Margin = new Thickness(20), RowDefinitions = new RowDefinitions("Auto,*,Auto,Auto"), RowSpacing = 12 };
        content.Children.Add(note);
        var scroll = new ScrollViewer { Content = sets, HorizontalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Disabled };
        Grid.SetRow(scroll, 1); content.Children.Add(scroll);
        Grid.SetRow(add, 2); content.Children.Add(add);
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Children = { cancel, apply } };
        Grid.SetRow(actions, 3); content.Children.Add(actions);
        dialog.Content = content;
        Populate();
        await dialog.ShowDialog(this);
    }
}

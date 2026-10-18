using System;
using System.Linq;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input.Platform;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform;

namespace VRPorts;
public partial class MainWindow
{
    Button? homeLaunchButton;
    Port? ActualPort => ports.FirstOrDefault(p => p.Id == S(state?["receipt"], "version")) ?? ports.FirstOrDefault(p => p.Id == "flat");
    void OpenCurrentLaunchSettings()
    {
        if (busy || ActualPort is not Port port) return;
        SelectPort(port); advanced = true; OpenManage();
    }
    Border QuietPanel(Control child) => new() { Child = child, Padding = new Thickness(18), CornerRadius = new CornerRadius(12),
        Background = Brush("#151F28"), BorderBrush = Brush("#30414D"), BorderThickness = new Thickness(1) };

    void BuildLibrary()
    {
        host.Children.Clear(); ReadGameProfiles();
        profilePicker = null; createProfileButton = null; repairProfileButton = null; gameCardButton = null;
        homeResultBox = null; homeResultText = null; homeLaunchButton = null;
        homeRedmodToggle = null; homeEnhancementToggle = null;
        testSavesToggle = null; testSaveSetButton = null; testSavesStatus = null;
        var root = new Grid { RowDefinitions = new RowDefinitions("Auto,Auto,*,Auto"), Margin = new Thickness(26) };
        var top = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto,Auto"), ColumnSpacing = 16, Margin = new Thickness(0,0,0,22) };
        var brand = Text("VR-центр", 23, "#F0F6F9"); brand.FontWeight = FontWeight.SemiBold;
        top.Children.Add(brand);
        var tools = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6, VerticalAlignment = VerticalAlignment.Center };
        if (showPorts) tools.Children.Add(Btn("← Игры", () => { showPorts = false; manage = false; BuildLibrary(); }, "SegmentedTab"));
        tools.Children.Add(Btn("Перевод модов", () => _ = TranslatorDialog(), "SegmentedTab"));
        tools.Children.Add(Btn("+ VR-порт", () => _ = ImportDialog(), "SegmentedTab"));
        Grid.SetColumn(tools,1); top.Children.Add(tools);
        var windowButtons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 4 };
        windowButtons.Children.Add(Btn("—", () => WindowState = WindowState.Minimized, "GlassWindowControlBtn"));
        windowButtons.Children.Add(Btn("□", () => WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized, "GlassWindowControlBtn"));
        windowButtons.Children.Add(Btn("×", Close, "GlassWindowControlBtn")); Grid.SetColumn(windowButtons,2); top.Children.Add(windowButtons);
        top.PointerPressed += (_,e) => { if(e.Source == brand && e.GetCurrentPoint(this).Properties.IsLeftButtonPressed) BeginMoveDrag(e); };
        root.Children.Add(top);
        var heading = Text(showPorts ? "Cyberpunk 2077  /  Варианты запуска" : "Библиотека",16,"#C3D4DF");
        heading.Margin = new Thickness(0,0,0,16); Grid.SetRow(heading,1); root.Children.Add(heading);
        Control content;
        if (!showPorts)
        {
            var library = new Grid { ColumnDefinitions = new ColumnDefinitions("310,24,*"), VerticalAlignment = VerticalAlignment.Top };
            var cover = new Border { Height = 240, CornerRadius = new CornerRadius(9), ClipToBounds = true,
                Child = new Image { Source = new Bitmap(AssetLoader.Open(new Uri("avares://VRPorts/Assets/vr-center-quest3-cyberpunk.png"))), Stretch = Stretch.UniformToFill } };
            gameCardButton = Btn("", () => { if (busy) return; showPorts = true; BuildLibrary(); }, "CleanCardBtn");
            gameCardButton.Content = cover; gameCardButton.HorizontalAlignment = HorizontalAlignment.Stretch;
            var card = QuietPanel(new StackPanel { Spacing = 12, Children = {
                BuildGameCardCover(gameCardButton), Text("Cyberpunk 2077",18,"#F3F8FB"), BuildGameProfileControls()
            } }); card.Padding = new Thickness(12); library.Children.Add(card);
            var launch = BuildQuickLaunch(); Grid.SetColumn(launch,2); library.Children.Add(launch); content = library;
        }
        else
        {
            var list = new StackPanel { Spacing = 9, MaxWidth = 1040, HorizontalAlignment = HorizontalAlignment.Stretch };
            foreach (var family in ports.Select(p => p.Family).Distinct().OrderBy(f => f == "Плоская игра" ? 0 : f == "Astra HD" ? 1 : f == "Сохранённые" ? 9 : 2))
            {
                string f = family; var current = ports.FirstOrDefault(p => p.Family == f && B(p.Node,"installed")); var chosen = current ?? DefaultPort(f);
                var row = new Grid { ColumnDefinitions = new ColumnDefinitions("38,*,Auto,Auto,Auto"), ColumnSpacing = 12 };
                var badge = new Border { Background = Brush(current != null ? "#203F36" : "#25323E"), CornerRadius = new CornerRadius(7), Width = 36, Height = 30,
                    VerticalAlignment = VerticalAlignment.Center, Child = Text(f == "Плоская игра" ? "2D" : "VR",12,current != null ? "#9FEECC" : "#ABC2D2") };
                if (badge.Child is TextBlock badgeText) { badgeText.HorizontalAlignment = HorizontalAlignment.Center; badgeText.VerticalAlignment = VerticalAlignment.Center; }
                row.Children.Add(badge);
                var title = Text(f,16,"#EEF5F8");
                var subtitle = new TextBlock { Text = (current != null ? "Подключён · " : "") + chosen.ToString(), FontSize = 11,
                    Foreground = Brush(current != null ? "#93DDBB" : "#97ACBA"), TextTrimming = TextTrimming.CharacterEllipsis };
                var open = Btn("", () => { SelectPort(chosen); advanced = false; OpenManage(); }, "CleanCardBtn");
                open.Content = new StackPanel { Spacing = 5, Children = { title, subtitle } };
                open.HorizontalAlignment = HorizontalAlignment.Stretch; open.HorizontalContentAlignment = HorizontalAlignment.Left; Grid.SetColumn(open,1); row.Children.Add(open);
                var configure = Btn("Открыть", () => { SelectPort(chosen); advanced = false; OpenManage(); }); Grid.SetColumn(configure,2); row.Children.Add(configure);
                var repo = RepoId(f); var update = Btn("↻", () => _ = UpdatesDialog(repo), "AppleSecondaryBtn", "Проверить обновления этого порта");
                update.IsVisible = repo != null; update.IsEnabled = !busy; Grid.SetColumn(update,3); row.Children.Add(update);
                var remove = Btn("⌫", () => _ = DeleteVersions(ports.Where(p => p.Family == f).ToArray()), "AppleDangerBtn", "Удалить порт из библиотеки");
                remove.IsVisible = current == null && f != "Плоская игра"; remove.IsEnabled = !busy; Grid.SetColumn(remove,4); row.Children.Add(remove);
                var card = QuietPanel(row); card.Padding = new Thickness(14,12); card.BorderBrush = Brush(current != null ? "#477C68" : "#2D3B47"); list.Children.Add(card);
            }
            content = list;
        }
        var scroll = new ScrollViewer { Content = content, HorizontalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Disabled };
        Grid.SetRow(scroll,2); root.Children.Add(scroll);
        homeResultText = Text(result,12,error ? "#FFB4AA" : "#9AE3C1");
        homeResultText.MaxWidth = 820;
        var status = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 12, Children = { homeResultText } };
        if (details.Length > 0) status.Children.Add(Btn("Подробнее", () => _ = StatusDetailsDialog(), "SegmentedTab"));
        homeResultBox = QuietPanel(status); homeResultBox.Padding = new Thickness(10,7); homeResultBox.Margin = new Thickness(0,14,0,0);
        homeResultBox.HorizontalAlignment = HorizontalAlignment.Left; Grid.SetRow(homeResultBox,3); root.Children.Add(homeResultBox);
        RefreshGameProfileControls();
        var background = new LinearGradientBrush { StartPoint = new RelativePoint(0,0,RelativeUnit.Relative), EndPoint = new RelativePoint(1,1,RelativeUnit.Relative),
            GradientStops = new GradientStops { new GradientStop(Color.Parse("#13202A"),0),new GradientStop(Color.Parse("#0D151C"),.7),new GradientStop(Color.Parse("#101D23"),1) } };
        host.Children.Add(new Border { Child = root, Background = background, CornerRadius = new CornerRadius(14), BorderBrush = Brush("#344650"), BorderThickness = new Thickness(1), Margin = new Thickness(4), ClipToBounds = true });
        if (manage) BuildManage();
    }

    Control BuildQuickLaunch()
    {
        var body = new StackPanel { Spacing = 18 };
        var currentSettings = Btn("Текущий запуск", OpenCurrentLaunchSettings, "SegmentedTab", "Расширенные настройки установленного режима");
        currentSettings.Name = "CurrentLaunchSettings";
        currentSettings.FontSize = 20; currentSettings.Padding = new Thickness(0); currentSettings.MinHeight = 0;
        currentSettings.HorizontalAlignment = HorizontalAlignment.Left;
        body.Children.Add(currentSettings);
        body.Children.Add(Text("Используется активный профиль и подключённый вариант игры.",12,"#91A9B8"));
        void Row(string label,string value,string color = "#DCEAF1")
        {
            var row = new Grid { ColumnDefinitions = new ColumnDefinitions("105,*"), ColumnSpacing = 10 };
            row.Children.Add(Text(label,12,"#91A9B8"));
            var text = Text(value,13,color); Grid.SetColumn(text,1); row.Children.Add(text); body.Children.Add(row);
        }
        Row("Профиль", ActiveGameProfileName, "#9DE5C4");
        Row("Режим", ActualPort?.Family ?? "Плоская игра");
        Row("Коллекция", S(state?["collection"],"kind") == "vortex" ? S(state?["collection"],"profileName") : S(state?["collection"],"kind") == "mo2" ? S(state?["collection"],"profile") : "Файлы игры");
        Row("Настройки", B(state,"recommendedSettings") ? "Пресет выбранного порта" : "Пользовательские");
        body.Children.Add(BuildTestSaveControls());
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Margin = new Thickness(0,4,0,0) };
        homeLaunchButton = Btn("Запустить", () => _ = Run("Launch"), "ApplePrimaryBtn", "Запустить текущее состояние. Неприменённые изменения не устанавливаются.");
        actions.Children.Add(homeLaunchButton); actions.Children.Add(Btn("Настройки игры", () => _ = GraphicsSettingsDialog()));
        body.Children.Add(actions);
        var choose = Btn("Выбрать другой VR-порт →", () => { showPorts = true; BuildLibrary(); }, "SegmentedTab");
        choose.HorizontalAlignment = HorizontalAlignment.Left; body.Children.Add(choose);
        var panel = QuietPanel(body); panel.MaxWidth = 650; panel.HorizontalAlignment = HorizontalAlignment.Left; panel.VerticalAlignment = VerticalAlignment.Top;
        return panel;
    }

    async Task StatusDetailsDialog()
    {
        var dialog = new Window { Title = "Подробности операции", Width = 720, Height = 420, Background = Brush("#111D26"), WindowStartupLocation = WindowStartupLocation.CenterOwner };
        var text = new TextBox { Text = details.Length > 0 ? details : result, IsReadOnly = true, AcceptsReturn = true, TextWrapping = TextWrapping.Wrap };
        var content = new Grid { Margin = new Thickness(20), RowDefinitions = new RowDefinitions("Auto,*,Auto"), RowSpacing = 12 };
        content.Children.Add(Text(result,14,error ? "#FFB4AA" : "#9AE3C1")); Grid.SetRow(text,1); content.Children.Add(text);
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        actions.Children.Add(Btn("Копировать", async () => { if (dialog.Clipboard != null) await dialog.Clipboard.SetTextAsync(text.Text ?? ""); }));
        actions.Children.Add(Btn("Закрыть", dialog.Close)); Grid.SetRow(actions,2); content.Children.Add(actions);
        dialog.Content = content; await dialog.ShowDialog(this);
    }
}

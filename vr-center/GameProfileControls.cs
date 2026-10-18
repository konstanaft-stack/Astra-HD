using System;
using System.Collections.Generic;
using System.Linq;

namespace VRPorts {
 public record GameProfileChoice(string Id, string Name, bool Vr, string? CreatedUtc = null) {
  public override string ToString() => Name;
  public string CreatedTooltip => DateTimeOffset.TryParse(CreatedUtc, System.Globalization.CultureInfo.InvariantCulture,
   System.Globalization.DateTimeStyles.RoundtripKind, out var stamp)
   ? "Создан: " + stamp.ToLocalTime().ToString("dd.MM.yyyy HH:mm:ss") : "Дата создания ещё не записана";
 }
 public enum GameProfileMarker { None, Active, Pending }

 // The backend snapshot owns actual selection. A request never changes ActiveId.
 public sealed class GameProfileSelection {
  public bool Available { get; private set; }
  public bool RecoveryPending { get; private set; }
  public string ActiveId { get; private set; } = "current";
  public string? PendingId { get; private set; }
  public IReadOnlyList<GameProfileChoice> Profiles { get; private set; } = Array.Empty<GameProfileChoice>();
  public void SetSnapshot(bool available, string activeId, IEnumerable<GameProfileChoice> profiles, bool recoveryPending = false) {
   Available = available;
   RecoveryPending = recoveryPending;
   ActiveId = activeId ?? "";
   Profiles = profiles.ToArray();
   PendingId = null;
  }
  public bool CanDelete(string id) => Available && !RecoveryPending && PendingId == null && id != ActiveId && Profiles.Any(p => p.Id == id);
  public bool BeginSwitch(string id) {
   if (!Available || RecoveryPending || PendingId != null || id == ActiveId || !Profiles.Any(p => p.Id == id)) return false;
   PendingId = id;
   return true;
  }
  public void CancelPending() => PendingId = null;
  public GameProfileMarker Marker(string id) => id == ActiveId ? GameProfileMarker.Active : id == PendingId ? GameProfileMarker.Pending : GameProfileMarker.None;
 }
}

// PROFILE_UI_CONTROLS
namespace VRPorts {
 public sealed class GameProfileChoiceRow : Avalonia.Controls.Grid {
  public Avalonia.Controls.Button RemoveButton { get; }
  public GameProfileChoiceRow(GameProfileChoice choice, bool active, bool canDelete, Action<string> remove, bool pending = false) {
   ColumnDefinitions = new Avalonia.Controls.ColumnDefinitions("Auto,*,Auto");
   ColumnSpacing = 7;
   var marker = new Avalonia.Controls.Border { Width = 7, Height = 7, CornerRadius = new Avalonia.CornerRadius(4),
    VerticalAlignment = Avalonia.Layout.VerticalAlignment.Center,
    Background = Avalonia.Media.Brush.Parse(active ? "#4ADE80" : pending ? "#5DAFFF" : "#00000000") };
   Children.Add(marker);
   Avalonia.Controls.ToolTip.SetTip(this, choice.CreatedTooltip);
   var name = new Avalonia.Controls.TextBlock {
    Text = choice.Name, FontSize = 12,
    TextTrimming = Avalonia.Media.TextTrimming.CharacterEllipsis,
    VerticalAlignment = Avalonia.Layout.VerticalAlignment.Center
   };
   Avalonia.Controls.Grid.SetColumn(name, 1); Children.Add(name);
   RemoveButton = new Avalonia.Controls.Button {
    Content = new Avalonia.Controls.TextBlock {
     Text = "\uE74D", FontFamily = new Avalonia.Media.FontFamily("Segoe MDL2 Assets"), FontSize = 13
    },
    Padding = new Avalonia.Thickness(5), MinWidth = 25, MinHeight = 25,
    IsVisible = !active, IsEnabled = canDelete, Classes = { "AppleDangerBtn" }
   };
   Avalonia.Controls.Grid.SetColumn(RemoveButton, 2);
   Avalonia.Automation.AutomationProperties.SetName(RemoveButton, "Удалить профиль «" + choice.Name + "»");
   Avalonia.Controls.ToolTip.SetShowOnDisabled(RemoveButton, true);
   Avalonia.Controls.ToolTip.SetTip(RemoveButton, active
    ? "Активный профиль нельзя удалить. Сначала выбери другой."
    : "Удалить профиль «" + choice.Name + "»");
   // Consume the pointer route before ComboBox can treat a trash click as item selection.
   RemoveButton.AddHandler(Avalonia.Input.InputElement.PointerPressedEvent, (_, e) => e.Handled = true,
    Avalonia.Interactivity.RoutingStrategies.Bubble, handledEventsToo: true);
   RemoveButton.AddHandler(Avalonia.Input.InputElement.PointerReleasedEvent, (_, e) => e.Handled = true,
    Avalonia.Interactivity.RoutingStrategies.Bubble, handledEventsToo: true);
   RemoveButton.Click += (_, e) => { e.Handled = true; if (RemoveButton.IsEnabled) remove(choice.Id); };
   Children.Add(RemoveButton);
  }
 }

 public sealed class GameProfilePicker : Avalonia.Controls.ComboBox {
  protected override Type StyleKeyOverride => typeof(Avalonia.Controls.ComboBox);
  readonly GameProfileSelection model;
  readonly Func<bool> blocked;
  bool synchronizing;
  public GameProfilePicker(GameProfileSelection model, Func<bool> blocked, Action<string> select, Action<string> remove) {
   this.model = model;
   this.blocked = blocked;
   MinWidth = 175;
   HorizontalAlignment = Avalonia.Layout.HorizontalAlignment.Stretch;
   Avalonia.Automation.AutomationProperties.SetName(this, "Профиль Cyberpunk 2077");
   Avalonia.Controls.ToolTip.SetShowOnDisabled(this, true);
   ItemsSource = model.Profiles;
   ItemTemplate = new Avalonia.Controls.Templates.FuncDataTemplate<GameProfileChoice>((choice, _) => choice == null ? null
    : new GameProfileChoiceRow(choice, choice.Id == model.ActiveId, model.CanDelete(choice.Id) && !blocked(),
     id => { if (!blocked() && model.CanDelete(id)) remove(id); }, choice.Id == model.PendingId));
   RestoreActualSelection();
   SelectionChanged += (_, _) => {
    if (synchronizing || SelectedItem is not GameProfileChoice choice) return;
    RestoreActualSelection();
    if (blocked() || !model.BeginSwitch(choice.Id)) return;
    IsDropDownOpen = false;
    Refresh();
    select(choice.Id);
   };
   Refresh();
  }
  void RestoreActualSelection() {
   synchronizing = true;
   SelectedItem = model.Profiles.FirstOrDefault(p => p.Id == model.ActiveId);
   synchronizing = false;
  }
  public void Refresh() => IsEnabled = model.Available && !model.RecoveryPending && model.PendingId == null && !blocked();
 }

 public partial class MainWindow {
  readonly GameProfileSelection gameProfileSelection = new();
  GameProfilePicker? profilePicker;
  Avalonia.Controls.Button? createProfileButton, duplicateProfileButton, gameCardButton, repairProfileButton;
  Avalonia.Controls.Border? homeResultBox;
  Avalonia.Controls.TextBlock? homeResultText;
  bool GameProfilesAvailable => B(state?["gameProfiles"], "available");
  bool GameProfilesEnabled => B(state?["gameProfiles"], "enabled") || GameProfilesAvailable;
  string ActiveGameProfileName => gameProfileSelection.Profiles.FirstOrDefault(p => p.Id == gameProfileSelection.ActiveId)?.Name ?? "Профиль недоступен";
  Avalonia.Controls.Border? profilePendingIndicator;
  Avalonia.Controls.Button? refreshPortStateButton;

  void ReadGameProfiles() {
   var profileState = state?["gameProfiles"];
   var choices = (profileState?["profiles"]?.AsArray() ?? new()).Select(p =>
    new GameProfileChoice(S(p, "id"), S(p, "name"), B(p, "vr"), S(p, "createdUtc"))).ToArray();
   gameProfileSelection.SetSnapshot(GameProfilesAvailable, S(profileState, "activeId"), choices,
    (profileState?["recoveryRequired"]?.AsArray()?.Count ?? 0) > 0);
  }

  Avalonia.Controls.Control BuildGameProfileControls() {
   profilePicker = new GameProfilePicker(gameProfileSelection, () => busy || loading,
    id => _ = Run("SwitchGameProfile", new System.Text.Json.Nodes.JsonObject { ["profileId"] = id }),
    id => _ = DeleteGameProfile(id));
   var unavailable = S(state?["gameProfiles"], "reason");
   Avalonia.Controls.ToolTip.SetTip(profilePicker, GameProfilesAvailable
    ? "Профиль игры. При переключении текущий состав автоматически сохраняется."
    : unavailable.Length > 0 ? unavailable : "Профили пока недоступны.");
   var row = new Avalonia.Controls.Grid { ColumnDefinitions = new Avalonia.Controls.ColumnDefinitions("*,Auto,Auto"), ColumnSpacing = 6 };
   row.Children.Add(profilePicker);
   profilePendingIndicator = new Avalonia.Controls.Border { Width = 7, Height = 7, CornerRadius = new Avalonia.CornerRadius(4),
    Background = Brush("#5DAFFF"), VerticalAlignment = Avalonia.Layout.VerticalAlignment.Center, IsVisible = false };
   Avalonia.Controls.Grid.SetColumn(profilePendingIndicator, 1); row.Children.Add(profilePendingIndicator);
   duplicateProfileButton = Btn("⧉", () => _ = CreateGameProfileDialog(true), "AppleSecondaryBtn", "Дублировать текущий профиль: отдельные настройки и сохранения, общие архивы модов.");
   duplicateProfileButton.Name = "DuplicateGameProfile";
   duplicateProfileButton.MinWidth = 30; duplicateProfileButton.Padding = new Avalonia.Thickness(7, 5);
   Avalonia.Automation.AutomationProperties.SetName(duplicateProfileButton, "Дублировать профиль");
   Avalonia.Controls.Grid.SetColumn(duplicateProfileButton, 2); row.Children.Add(duplicateProfileButton);
   repairProfileButton = Btn("Восстановить профиль", () => _ = Run("RepairGameProfiles"), "AppleDangerBtn",
    "Восстановить согласованное состояние после прерванного переключения. Изменённые посторонней программой файлы не перезаписываются.");
   refreshPortStateButton = Btn("Обновить состояние", () => _ = Run("Status"), "AppleSecondaryBtn",
    "Повторно прочитать полный состав игры. После ошибки старые данные порта не используются.");
   var controls = new Avalonia.Controls.StackPanel { Spacing = 7, Children = { row, BuildCollectionControls(), repairProfileButton, refreshPortStateButton } };
   if(!B(state?["cleanBaseline"],"captured")){
    var capture=Btn("Сохранить чистую основу",async()=>{
     if(await Confirm("Чистая основа", "Игра заново установлена без модов, один раз запущена до главного меню и сейчас закрыта? Сохраним её исходные файлы, AppData и Saved Games для новых профилей."))
      await Run("CaptureCleanBaseline",new System.Text.Json.Nodes.JsonObject{["firstLaunchConfirmed"]=true});
    },"AppleSecondaryBtn","Зафиксировать исходное состояние один раз после переустановки и первого меню.");
    capture.IsEnabled=!busy&&!loading&&!B(state,"gameRunning")&&!B(state,"vortexRunning");controls.Children.Add(capture);
   }
   RefreshGameProfileControls();
   return controls;
  }

  Avalonia.Controls.Control BuildGameCardCover(Avalonia.Controls.Button coverButton) {
   var grid = new Avalonia.Controls.Grid(); grid.Children.Add(coverButton);
   createProfileButton = Btn("+", () => _ = CreateGameProfileDialog(), "AppleSecondaryBtn", "Создать новый чистый профиль игры");
   createProfileButton.Classes.Add("CoverAction");
   createProfileButton.Padding = new Avalonia.Thickness(7); createProfileButton.MinWidth = 30;
   createProfileButton.HorizontalAlignment = Avalonia.Layout.HorizontalAlignment.Right;
   createProfileButton.VerticalAlignment = Avalonia.Layout.VerticalAlignment.Top;
   createProfileButton.Margin = new Avalonia.Thickness(10);
   Avalonia.Automation.AutomationProperties.SetName(createProfileButton, "Создать профиль Cyberpunk 2077");
   Avalonia.Controls.ToolTip.SetShowOnDisabled(createProfileButton, true);
   createProfileButton.Click += (_, e) => e.Handled = true;
   grid.Children.Add(createProfileButton);
   var settings = Btn("⚙", () => _ = GraphicsSettingsDialog(), "AppleSecondaryBtn", "Настройки игры перед запуском: генерация кадров, звук, графика");
   settings.Classes.Add("CoverAction"); settings.Padding = new Avalonia.Thickness(7);
   settings.HorizontalAlignment = Avalonia.Layout.HorizontalAlignment.Left; settings.VerticalAlignment = Avalonia.Layout.VerticalAlignment.Top;
   settings.Margin = new Avalonia.Thickness(10); settings.Click += (_, e) => e.Handled = true; grid.Children.Add(settings);
   return grid;
  }

  void RefreshGameProfileControls() {
   RefreshCurrentLaunchControls();
   RefreshTestSaveControls();
   profilePicker?.Refresh();
   if (profilePendingIndicator != null) profilePendingIndicator.IsVisible = gameProfileSelection.PendingId != null;
   if (createProfileButton != null) createProfileButton.IsEnabled = GameProfilesAvailable && !gameProfileSelection.RecoveryPending && !busy && !loading;
   if (duplicateProfileButton != null) duplicateProfileButton.IsEnabled = GameProfilesAvailable && !gameProfileSelection.RecoveryPending && !busy && !loading && !B(state,"gameRunning") && !B(state,"vortexRunning");
   if (gameCardButton != null) gameCardButton.IsEnabled = portStateCurrent && !busy && !gameProfileSelection.RecoveryPending && gameProfileSelection.PendingId == null;
   if (homeLaunchButton != null) homeLaunchButton.IsEnabled = portStateCurrent && !busy && !loading && B(state,"exePresent") && !B(state,"hold") && !B(state,"gameRunning") && !B(state,"vortexRunning") && !gameProfileSelection.RecoveryPending && gameProfileSelection.PendingId == null;
   if (refreshPortStateButton != null) {
    refreshPortStateButton.IsVisible = !portStateCurrent;
    refreshPortStateButton.IsEnabled = !busy && !loading;
   }
   if (repairProfileButton != null) {
    repairProfileButton.IsVisible = gameProfileSelection.RecoveryPending;
    repairProfileButton.IsEnabled = GameProfilesAvailable && !busy && !loading;
   }
   if (homeResultText != null) {
    homeResultText.Text = result;
    homeResultText.Foreground = Brush(error ? "#FF9A90" : "#9BEAB4");
   }
   if (homeResultBox != null) homeResultBox.IsVisible = result.Length > 0;
  }

  async System.Threading.Tasks.Task DeleteGameProfile(string id) {
   if (busy || !gameProfileSelection.CanDelete(id)) return;
   var profile = gameProfileSelection.Profiles.First(p => p.Id == id);
   profilePicker!.IsDropDownOpen = false;
   if (await Confirm("Удалить профиль?", "Удалить профиль «" + profile.Name + "» и его собственные данные?")) {
    if (!busy && gameProfileSelection.CanDelete(id))
     await Run("DeleteGameProfile", new System.Text.Json.Nodes.JsonObject { ["profileId"] = id });
   }
  }

  async System.Threading.Tasks.Task CreateGameProfileDialog(bool duplicate = false) {
   if (busy || !GameProfilesAvailable || gameProfileSelection.RecoveryPending) return;
   var dialog = new Avalonia.Controls.Window {
    Title = duplicate ? "Дублировать профиль" : "Новый профиль Cyberpunk 2077", Width = 430, Height = 245,
    WindowStartupLocation = Avalonia.Controls.WindowStartupLocation.CenterOwner, Background = Brush("#101C15")
   };
   var name = new Avalonia.Controls.TextBox { PlaceholderText = "Название профиля", Text = duplicate ? ActiveGameProfileName + " — копия" : "" };
   Avalonia.Automation.AutomationProperties.SetName(name, "Название нового профиля");
   var modeHint = Text(duplicate ? "Текущий состав, настройки и сохранения. Архивы общие." : "Чистая базовая игра и свежие настройки.", 12, "#A5B9AC");
   var note = Text("Текущий профиль автоматически сохранится под прежним именем.", 11, "#A5B9AC");
   var cancel = Btn("Отмена", dialog.Close);
   Avalonia.Controls.Button create = null!;
   create = Btn(duplicate ? "Дублировать" : "Создать", async () => {
    if (busy) return;
    if (string.IsNullOrWhiteSpace(name.Text)) { note.Text = "Укажи название профиля."; name.Focus(); return; }
    name.IsEnabled = cancel.IsEnabled = create.IsEnabled = false;
    note.Text = "Сохраняю текущий профиль и создаю новый…";
    await Run(duplicate ? "DuplicateGameProfile" : "CreateGameProfile", new System.Text.Json.Nodes.JsonObject { ["name"] = name.Text.Trim(), ["vr"] = false });
    if (!error) { dialog.Close(); return; }
    note.Text = result;
    note.Foreground = Brush("#FF9A90");
    name.IsEnabled = cancel.IsEnabled = create.IsEnabled = true;
   }, "ApplePrimaryBtn");
   dialog.Closing += (_, e) => { if (busy) e.Cancel = true; };
   dialog.Content = new Avalonia.Controls.StackPanel {
    Margin = new Avalonia.Thickness(24), Spacing = 14,
    Children = { name, modeHint, note,
     new Avalonia.Controls.StackPanel { Orientation = Avalonia.Layout.Orientation.Horizontal, Spacing = 10, Children = { cancel, create } } }
   };
   await dialog.ShowDialog(this);
  }
 }
}

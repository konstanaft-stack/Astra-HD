using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Headless;
using Avalonia.Input;
using Avalonia.VisualTree;
using VRPorts;

// Root may run this isolated headless harness. No MainWindow, config, backend or real game paths.
bool render = args.Length == 2 && args[0] == "--render";
var builder = AppBuilder.Configure<App>();
if (render) builder = builder.UseSkia().WithInterFont();
builder.UseHeadless(new AvaloniaHeadlessPlatformOptions { UseHeadlessDrawing = !render }).SetupWithoutStarting();
if (render) { UiPreview.Run(args[1]); return; }
if (args.Length == 2 && args[0] == "--vortex-collection") { VortexCollectionChecks.Run(args[1]); return; }
if (args.Length == 2 && args[0] == "--test-saves") { TestSaveUiChecks.Run(args[1]); return; }
if (args.Length == 2 && args[0] == "--current-launch") { CurrentLaunchChecks.Run(args[1]); return; }
if (args.Length == 2 && args[0] == "--shared-enhancement") { SharedEnhancementChecks.Run(args[1]); return; }
var profiles = new GameProfileSelection();
profiles.SetSnapshot(true, "A", new[] {
    new GameProfileChoice("A", "Текущий", true),
    new GameProfileChoice("B", "Сохранённый", false)
});
int switches = 0, deletes = 0;
string? deletedId = null;
var picker = new GameProfilePicker(profiles, () => false, _ => switches++, id => { deletes++; deletedId = id; });
picker.TemplateApplied += (_, e) => {
    var popup = e.NameScope.Find<Popup>("PART_Popup");
    if (popup != null) popup.ShouldUseOverlayLayer = true;
};
var artwork = new Button { Content = "Cyberpunk 2077", Height = 80 };
int enters = 0;
artwork.Click += (_, _) => enters++;
var window = new Window {
    Width = 380, Height = 220,
    Content = new StackPanel { Margin = new Thickness(20), Spacing = 12, Children = { artwork, picker } }
};
window.Show();
window.UpdateLayout();
picker.IsDropDownOpen = true;
window.UpdateLayout();
var inactiveItem = picker.ContainerFromIndex(1) as ComboBoxItem ?? throw new Exception("Inactive popup item was not realized");
var inactiveRow = inactiveItem.GetVisualDescendants().OfType<GameProfileChoiceRow>().FirstOrDefault()
    ?? throw new Exception("Production dropdown row was not realized");
var trash = inactiveRow.RemoveButton;
void Click(Control control) {
    // Popup positioning is dispatched after realization. Before this pump the
    // inactive trash translated to (312.5,62.5), then moved to (332.5,83.5)
    // during MouseDown, so the fixture accidentally clicked the item name.
    Avalonia.Threading.Dispatcher.UIThread.RunJobs();
    window.UpdateLayout();
    var targetRoot = TopLevel.GetTopLevel(control) ?? throw new Exception("Pointer target has no TopLevel");
    var point = control.TranslatePoint(new Point(control.Bounds.Width / 2, control.Bounds.Height / 2), targetRoot)
        ?? throw new Exception("Pointer target cannot translate to its own TopLevel");
    if (control is Button && (targetRoot.InputHitTest(point) is not Visual hit ||
        (hit != control && !hit.GetVisualAncestors().Contains(control))))
        throw new Exception("Trash test coordinate does not hit the real production Button");
    // ComboBox may create PopupRoot. Headless input must target the actual TopLevel,
    // not coordinates translated to the owner while the native popup is elsewhere.
    targetRoot.MouseMove(point);
    targetRoot.MouseDown(point, MouseButton.Left);
    targetRoot.MouseUp(point, MouseButton.Left);
    targetRoot.UpdateLayout();
}
Click(trash);
if (deletes != 1 || deletedId != "B" || switches != 0 || enters != 0 || profiles.ActiveId != "A" || profiles.PendingId != null)
    throw new Exception($"Trash pointer click leaked to selection/card: delete={deletes}, switch={switches}, enter={enters}, actual={profiles.ActiveId}, pending={profiles.PendingId}");
var activeItem = picker.ContainerFromIndex(0) as ComboBoxItem ?? throw new Exception("Active popup item was not realized");
var activeRow = activeItem.GetVisualDescendants().OfType<GameProfileChoiceRow>().First();
if (activeRow.RemoveButton.IsVisible || activeRow.RemoveButton.IsEnabled)
    throw new Exception("Active profile exposes an actionable trash button");
Click(inactiveRow.Children.OfType<TextBlock>().First());
if (switches != 1 || profiles.PendingId != "B" || profiles.ActiveId != "A" || picker.IsEnabled || enters != 0)
    throw new Exception("Name click did not stage exactly one request while retaining actual profile");
if ((picker.SelectedItem as GameProfileChoice)?.Id != "A") throw new Exception("Dropdown optimistically changed actual profile");
profiles.CancelPending();
picker.Refresh();
if (!picker.IsEnabled || (picker.SelectedItem as GameProfileChoice)?.Id != "A") throw new Exception("Backend failure did not restore actual selection");
window.Close();
Console.WriteLine("PASS: trash pointer route deletes inactive only; name click requests switch once; active selection waits for backend; sibling artwork receives no profile clicks.");
if (args.Length == 1) StartupRecoveryChecks.Run(args[0]);
if (args.Length == 2 && args[0] == "--collection") CollectionImportChecks.Run(args[1]);

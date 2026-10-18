using Avalonia;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Platform;
using System.Runtime.InteropServices;
using VRPorts;

if (args.Length != 1) throw new ArgumentException("Output directory is required");
Directory.CreateDirectory(args[0]);
AppBuilder.Configure<App>().WithInterFont().UseSkia()
    .UseHeadless(new AvaloniaHeadlessPlatformOptions { UseHeadlessDrawing = false })
    .SetupWithoutStarting();

bool usb = true, opti = false, staged = true;
int clicks = 0;
FeatureToggleButton Make(string name, Func<bool> selected, Func<bool> actual, Action<bool> set,
    bool available = true, string? alternate = null) => new(
        new FeatureToggleBinding(() => new(name, alternate, selected(), actual(), available), set),
        () => false, "Короткая подсказка о назначении функции.", "Компонент недоступен.");
var usbButton = Make("USB-восстановление", () => usb, () => usb, value => { usb = value; clicks++; });
var offButton = Make("OptiScaler", () => opti, () => opti, value => opti = value);
var pendingButton = Make("Генерация кадров", () => staged, () => false, value => staged = value);
var modeButton = Make("Рекомендуемые настройки", () => false, () => false, _ => {}, alternate: "Настройки игры");
var disabled = Make("Мод контроллеров", () => false, () => false, _ => throw new Exception("Disabled click"), false);
var lockedMode = new FeatureToggleButton(
    new FeatureToggleBinding(() => new("Рекомендуемые настройки", "Настройки игры", false, false, true),
        _ => throw new Exception("Locked mode click")), () => true, "Режим настроек", "Нет функции", () => "Идёт работа с Vortex.");
var lockedPendingNr = new FeatureToggleButton(
    new FeatureToggleBinding(() => new("DLSS NR", null, false, true, true),
        _ => throw new Exception("Locked NR click")), () => true, "Зависит от OptiScaler", "Нет функции", () => "Включи OptiScaler.");
var row = new WrapPanel { Orientation = Orientation.Horizontal, ItemSpacing = 14, LineSpacing = 14 };
foreach (var button in new[] { usbButton, offButton, pendingButton, modeButton, disabled, lockedMode, lockedPendingNr }) row.Children.Add(button);
var window = new Window {
    Width = 830, Height = 220,
    Background = Brush.Parse("#0D1911"),
    Content = new StackPanel {
        Margin = new Thickness(28), Spacing = 22,
        Children = {
            new TextBlock { Text = "VR-центр · проверка кнопок", Foreground = Brushes.White, FontSize = 21 },
            row
        }
    }
};
window.Show();
(int Green, int Red, int Blue, double MeanRed) Capture(string name) {
    using var frame = window.CaptureRenderedFrame() ?? throw new Exception("No rendered frame");
    frame.Save(Path.Combine(args[0], name + ".png"));
    using var pixels = frame.Lock();
    bool rgba = pixels.Format == PixelFormat.Rgba8888;
    if (!rgba && pixels.Format != PixelFormat.Bgra8888) throw new Exception("Unexpected capture format");
    var bytes = new byte[pixels.RowBytes * pixels.Size.Height];
    Marshal.Copy(pixels.Address, bytes, 0, bytes.Length);
    int green = 0, red = 0, blue = 0; long redTotal = 0;
    for (int y = 0; y < pixels.Size.Height; y++) for (int x = 0; x < pixels.Size.Width; x++) {
        int offset = y * pixels.RowBytes + x * 4;
        int r = bytes[offset + (rgba ? 0 : 2)], g = bytes[offset + 1], b = bytes[offset + (rgba ? 2 : 0)];
        if (g > 50 && g > r * 1.4 && g > b * 1.2) green++;
        if (r > 50 && r > g * 1.4 && r > b * 1.2) { red++; redTotal += r; }
        if (b > 50 && b > r * 1.4 && b > g * 1.2) blue++;
    }
    return (green, red, blue, red == 0 ? 0 : (double)redTotal / red);
}
Point Center(Control control) => control.TranslatePoint(new Point(control.Bounds.Width / 2, control.Bounds.Height / 2), window)
    ?? throw new Exception("Control is not in window");
var initialPixels = Capture("01-default");
if (initialPixels.Green < 400 || initialPixels.Red < 250 || initialPixels.Blue < 400)
    throw new Exception($"Colored tile surfaces missing: {initialPixels}");
if (usbButton.Content is not TextBlock || !usbButton.Classes.Contains("On") || !offButton.Classes.Contains("Off") ||
    !pendingButton.Classes.Contains("Pending") || !modeButton.Classes.Contains("On") || disabled.IsEnabled)
    throw new Exception("Initial rendered state mismatch");
if (!lockedMode.Classes.Contains("On") || lockedMode.IsEnabled || !lockedPendingNr.Classes.Contains("Pending") || lockedPendingNr.IsEnabled)
    throw new Exception("Temporary edit guard lost actual or pending color");
if (!ToolTip.GetTip(lockedMode)!.ToString()!.Contains("Vortex")) throw new Exception("Temporary edit guard reason missing");
window.MouseMove(Center(offButton));
var hoverPixels = Capture("02-red-hover");
if (hoverPixels.MeanRed <= initialPixels.MeanRed + 8)
    throw new Exception("Red tile does not become brighter on hover");
window.MouseDown(Center(offButton), MouseButton.Left);
Capture("03-red-pressed");
window.MouseUp(Center(offButton), MouseButton.Left);
if (!opti || !offButton.Classes.Contains("On")) throw new Exception("Real mouse click did not enable component");
window.MouseMove(Center(usbButton));
window.MouseDown(Center(usbButton), MouseButton.Left);
window.MouseUp(Center(usbButton), MouseButton.Left);
if (usb || clicks != 1 || !usbButton.Classes.Contains("Off")) throw new Exception("USB click/state mismatch");
window.MouseMove(new Point(800, 195));
Capture("04-after-clicks");
window.MouseMove(Center(lockedPendingNr));
window.MouseDown(Center(lockedPendingNr), MouseButton.Left);
window.MouseUp(Center(lockedPendingNr), MouseButton.Left);
if (!lockedPendingNr.Classes.Contains("Pending")) throw new Exception("Disabled pending click changed state");
window.Close();
Console.WriteLine("PASS: real Avalonia styles rendered; named tiles only; mouse toggles exactly once; four PNGs saved.");

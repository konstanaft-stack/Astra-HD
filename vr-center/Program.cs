using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using System.Diagnostics;
using System.Runtime.InteropServices;
namespace VRPorts;
internal static class Program
{
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] static extern bool ShowWindowAsync(IntPtr window,int command);
    public static string Root { get; private set; } = "";
    public static string CodeRoot => File.Exists(Path.Combine(AppContext.BaseDirectory,"Backend.ps1")) ? AppContext.BaseDirectory : Root;
    [STAThread] public static void Main(string[] args)
    {
        using var single=new Mutex(true,"Local\\VRPorts.UI.v1",out var created);
        if(!created){foreach(var p in Process.GetProcessesByName("VRPorts")){if(p.Id!=Environment.ProcessId&&p.MainWindowHandle!=IntPtr.Zero){ShowWindowAsync(p.MainWindowHandle,9);SetForegroundWindow(p.MainWindowHandle);break;}}return;}
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir != null && !File.Exists(Path.Combine(dir.FullName, "config.json"))) dir = dir.Parent;
        Root = dir?.FullName ?? throw new Exception("config.json не найден");
        try { AppBuilder.Configure<App>().UsePlatformDetect().WithInterFont().StartWithClassicDesktopLifetime(args); }
        catch(Exception ex) { File.WriteAllText(Path.Combine(Root,"startup-error.log"),ex.ToString()); throw; }
    }
}
public partial class App : Application
{
    public override void Initialize() => AvaloniaXamlLoader.Load(this);
    public override void OnFrameworkInitializationCompleted()
    {
        if(ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop) desktop.MainWindow = new MainWindow();
        base.OnFrameworkInitializationCompleted();
    }
}

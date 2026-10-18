using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using System.Diagnostics;
using System.Text.Json.Nodes;
namespace VRPorts;
public partial class MainWindow
{
 async Task TranslatorDialog(){
  var folder=Path.GetFullPath(Path.Combine(Program.Root,"..","Cyberpunk-Mod-Translator"));
  var exe=Path.Combine(folder,"1.0.2","Cyberpunk Mod Translator","Cyberpunk Mod Translator.exe");
  var runtime=Path.Combine(folder,"runtime");
  var output=Path.Combine(folder,"translations");
  var installed=File.Exists(exe)&&File.Exists(Path.Combine(runtime,"dotnet.exe"));
  var dialog=new Window{Title="Перевод модов · VR-центр",Width=630,Height=380,WindowStartupLocation=WindowStartupLocation.CenterOwner,Background=Brush("#101C15")};
  var status=Text("Cyberpunk Mod Translator 1.0.2",14,installed?"#86EFAC":"#FF9A90");
  var launch=Btn("Открыть переводчик",()=>{
   try{
    Directory.CreateDirectory(output);
    var start=new ProcessStartInfo(exe){WorkingDirectory=Path.GetDirectoryName(exe)!,UseShellExecute=false,CreateNoWindow=true};
    start.Environment["DOTNET_ROOT"]=runtime;start.Environment["DOTNET_ROOT_X64"]=runtime;
    start.Environment["PATH"]=runtime+Path.PathSeparator+Environment.GetEnvironmentVariable("PATH");
    var connectionPath=Path.Combine(folder,"connection.json");
    if(File.Exists(connectionPath)){
     var connection=JsonNode.Parse(File.ReadAllText(connectionPath));var apiBase=connection?["geminiBaseUrl"]?.GetValue<string>();
     if(!string.IsNullOrWhiteSpace(apiBase)){
      if(!Uri.TryCreate(apiBase,UriKind.Absolute,out var uri)||uri.Scheme!="https")throw new InvalidOperationException("Адрес Gemini должен использовать HTTPS.");
      start.Environment["GOOGLE_GEMINI_BASE_URL"]=apiBase;
     }
    }
    Process.Start(start);status.Text="Переводчик открыт. Выбери архив мода и язык в его окне.";
   }catch(Exception e){status.Text=e.Message;status.Foreground=Brush("#FF9A90");}
  },"ApplePrimaryBtn");launch.IsEnabled=installed;
  dialog.Content=new StackPanel{Margin=new Thickness(24),Spacing=16,Children={Text("Перевод модов",23),status,
   Card(Stack(Text("Архив мода → русский перевод → отдельный ZIP",15),Text("Выбери .archive или ZIP в переводчике. Готовый ZIP можно установить через Vortex.",13))),
   new WrapPanel{Orientation=Orientation.Horizontal,Children={launch,Btn("Папка переводов",()=>{Directory.CreateDirectory(output);Process.Start(new ProcessStartInfo(output){UseShellExecute=true});})}},
   Text("Перевод запускается кнопкой в самом переводчике.",12,"#A5B8AD"),Btn("Закрыть",()=>dialog.Close())}};
  await dialog.ShowDialog(this);
 }
}

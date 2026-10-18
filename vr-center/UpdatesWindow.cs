using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using System.Text.Json.Nodes;
namespace VRPorts;
public record UpdateChoice(string Label,JsonNode Value){public override string ToString()=>Label;}
public partial class MainWindow
{
 async Task UpdatesDialog(string? contextRepo=null){
  if(busy)return;
  var dialog=new Window{Title="Обновления VR-портов",Width=790,Height=650,MinWidth=650,WindowStartupLocation=WindowStartupLocation.CenterOwner,Background=Brush("#101C15")};
  var repo=new ComboBox();var release=new ComboBox();var asset=new ComboBox();var info=Text("",13);var status=Text("",12);var download=Btn("Скачать и добавить",()=>{});var import=Btn("Открыть версию",()=>{});
  Button? refresh=null,nvidia=null;
  void Availability(bool work){import.Content=S(state,"lastDownloadedVersionId")!=""?"Открыть версию":"Добавить архив";repo.IsEnabled=!work&&contextRepo==null;release.IsEnabled=!work;asset.IsEnabled=!work;download.IsEnabled=!work&&asset.SelectedItem!=null;import.IsEnabled=!work&&state?["lastPortDownload"]!=null&&(contextRepo==null||S(state["lastPortDownload"],"repoId")==contextRepo);if(refresh!=null)refresh.IsEnabled=!work;if(nvidia!=null)nvidia.IsEnabled=!work;}
  void Populate(){var current=(repo.SelectedItem as UpdateChoice)?.Value["id"]?.ToString();var rows=(state?["portUpdates"]?["repositories"]?.AsArray()??new()).Where(n=>contextRepo==null||S(n,"id")==contextRepo).Select(n=>new UpdateChoice(S(n,"name"),n!)).ToArray();repo.ItemsSource=rows;repo.SelectedItem=rows.FirstOrDefault(r=>S(r.Value,"id")==current)??rows.FirstOrDefault();Availability(false);}
  repo.SelectionChanged+=(_,_)=>{var r=(repo.SelectedItem as UpdateChoice)?.Value;if(r==null)return;var list=(r["releases"]?.AsArray()??new()).Select(n=>new UpdateChoice(S(n,"tag")+(B(n,"prerelease")?" • предварительный":""),n!)).ToArray();release.ItemsSource=list;release.SelectedIndex=list.Length>0?0:-1;info.Text=S(r,"error")!=""?S(r,"error"):$"В локальном каталоге: {S(r,"local")}\nПоследний публичный выпуск: {(list.Length>0?list[0].Label:"нет публичных выпусков")}";};
  release.SelectionChanged+=(_,_)=>{var r=(release.SelectedItem as UpdateChoice)?.Value;asset.ItemsSource=(r?["assets"]?.AsArray()??new()).Select(n=>new UpdateChoice(S(n,"name")+" • "+((n?["size"]?.GetValue<long>()??0)/1048576.0).ToString("F1")+" МБ",n!)).ToArray();asset.SelectedIndex=0;Availability(false);};asset.SelectionChanged+=(_,_)=>Availability(false);
  refresh=Btn("Проверить GitHub",async()=>{Availability(true);status.Text="Проверяю публичные релизы…";await Run("CheckUpdates",new JsonObject{["repoId"]=contextRepo});status.Text=result;status.Foreground=Brush(error?"#FF9A90":"#86EFAC");Populate();},"ApplePrimaryBtn");
  nvidia=Btn("Обновить каталог NVIDIA",async()=>{Availability(true);status.Text="Проверяю SDK NVIDIA и библиотеки…";await Run("UpdateLibraries");status.Text=error?result:"Каталог NVIDIA обновлён. Файлы игры не заменялись.";status.Foreground=Brush(error?"#FF9A90":"#86EFAC");Availability(false);});
  download.Click+=async(_,_)=>{var r=(repo.SelectedItem as UpdateChoice)?.Value;var rel=(release.SelectedItem as UpdateChoice)?.Value;var a=(asset.SelectedItem as UpdateChoice)?.Value;if(r==null||rel==null||a==null)return;Availability(true);status.Text="Скачиваю и добавляю выпуск в список…";await Run("DownloadAndImport",new JsonObject{["repoId"]=S(r,"id"),["tag"]=S(rel,"tag"),["assetId"]=a["id"]!.DeepClone()});if(!error){dialog.Close();return;}status.Text=result;status.Foreground=Brush("#FF9A90");Availability(false);};
  import.Click+=async(_,_)=>{var existing=ports.FirstOrDefault(p=>p.Id==S(state,"lastDownloadedVersionId"));if(existing!=null){SelectPort(existing);showPorts=true;OpenManage();dialog.Close();return;}Availability(true);status.Text="Добавляю скачанный выпуск в каталог…";await Run("ImportDownloaded",new JsonObject{["repoId"]=contextRepo});if(!error){dialog.Close();return;}status.Text=result;status.Foreground=Brush("#FF9A90");Populate();};
  var buttons=new StackPanel{Orientation=Orientation.Horizontal,Spacing=10,Children={refresh,nvidia}};
  dialog.Content=new StackPanel{Margin=new Thickness(24),Spacing=14,Children={Text("Обновления с GitHub",23),buttons,repo,info,release,asset,new StackPanel{Orientation=Orientation.Horizontal,Spacing=10,Children={download,import}},status,Btn("Закрыть",()=>{if(!busy)dialog.Close();})}};
  dialog.Closing+=(_,e)=>{if(busy)e.Cancel=true;};Populate();if(contextRepo!=null&&repo.ItemCount==0){await Run("CheckUpdates",new JsonObject{["repoId"]=contextRepo});Populate();}await dialog.ShowDialog(this);
 }
}

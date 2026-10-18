using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using System.Text.Json.Nodes;
namespace VRPorts;
public record MenuKey(int Code,string Label){public override string ToString()=>Label;}
public partial class MainWindow
{
 async Task KeysDialog(){
  if(selected==null||busy)return;var port=selected;
  var window=new Window{Title="Клавиши меню · "+port.Family,Width=530,Height=320,WindowStartupLocation=WindowStartupLocation.CenterOwner,Background=Brush("#101C15")};
  var current=state?["keyBindings"]?["ports"]?[port.Id]?["optiKey"]?.GetValue<int>()??(B(port.Node,"installed")?state?["optiMenuKey"]?.GetValue<int>():null)??117;
  var keys=new[]{new MenuKey(45,"Insert"),new MenuKey(117,"F6"),new MenuKey(118,"F7"),new MenuKey(119,"F8"),new MenuKey(120,"F9"),new MenuKey(121,"F10"),new MenuKey(122,"F11"),new MenuKey(123,"F12"),new MenuKey(19,"Pause")};
  var select=new ComboBox{ItemsSource=keys,SelectedItem=keys.FirstOrDefault(k=>k.Code==current)??keys[1]};
  var fixedPort=port.Family is "Astra HD" or "ToFu"?"F10 / Insert — задано кодом порта":"Задаётся самим портом";
  var status=Text("",12);var save=Btn("Сохранить",async()=>{if(select.SelectedItem is not MenuKey key)return;await Run("SaveKeys",new JsonObject{["version"]=port.Id,["optiKey"]=key.Code});status.Text=error?result:details;},"ApplePrimaryBtn");
  save.IsEnabled=B(port.Node,"allowOpti")||(B(port.Node,"installed")&&B(state?["receipt"],"opti"));
  var content=new StackPanel{Margin=new Thickness(24),Spacing=14,Children={Text("Клавиши меню",21),Text(port.Family+": "+fixedPort,13)}};if(save.IsEnabled){content.Children.Add(Text("Меню OptiScaler",14));content.Children.Add(select);content.Children.Add(status);content.Children.Add(save);}content.Children.Add(Btn("Закрыть",()=>window.Close()));window.Content=content;
  ToolTip.SetTip(select,"Клавиша открытия меню OptiScaler. Insert/F10 также открывают меню ToFu; для раздельного управления выбери другую.");await window.ShowDialog(this);
 }
}

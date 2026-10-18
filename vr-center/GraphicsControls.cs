using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.Json.Nodes;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;

namespace VRPorts;
public partial class MainWindow
{
    record GraphicsChoice(JsonNode? Value, string Label) { public override string ToString() => Label; }
    static string GraphicsValueLabel(string value) => value switch {
        "Off" => "Выключено", "On" => "Включено", "Auto" => "Автоматически", "PrioritizePCores" => "Приоритет P-ядер", "Quality" => "Качество", "Balanced" => "Баланс",
        "Performance" => "Производительность", "Ultra Performance" => "Макс. производительность",
        "Low" => "Низкое", "Medium" => "Среднее", "High" => "Высокое", "Ultra" => "Ультра", _ => value
    };
    async Task GraphicsSettingsDialog()
    {
        if (busy) return;
        await Run("ReadGraphicsSettings"); if (error) return;
        var dialog = new Window { Title="Настройки Cyberpunk 2077",Width=650,Height=670,
            MaxHeight=Math.Max(400,Bounds.Height-40),Background=Brush("#111D26"),WindowStartupLocation=WindowStartupLocation.CenterOwner };
        var fields = new Dictionary<string,(JsonNode? original,Func<JsonNode?> read,Action<JsonNode?> set)>();
        var presetButtons = new List<Button>();
        var form = new StackPanel { Spacing=11 };
        var note = Text("",12,"#9AB4C4");
        Button save=null!;
        JsonObject Changes()
        {
            var changes=new JsonObject();
            foreach(var pair in fields){var value=pair.Value.read();if(!JsonNode.DeepEquals(value,pair.Value.original))changes[pair.Key]=value?.DeepClone();}
            return changes;
        }
        void Pending()
        {
            int count=Changes().Count;
            note.Text=count>0?$"Изменений: {count}. Сохрани, чтобы применить к следующему запуску.":"Сохранённые настройки игры для следующего запуска.";
            note.Foreground=Brush(count>0?"#90C8FF":"#9AB4C4");
            if(save!=null)save.IsEnabled=count>0&&!busy&&!B(state,"gameRunning")&&!B(state,"vortexRunning");
        }
        decimal Number(JsonNode? value,decimal fallback=0)=>decimal.TryParse(value?.ToString(),NumberStyles.Float,CultureInfo.InvariantCulture,out var number)?number:fallback;
        void Populate()
        {
            fields.Clear();form.Children.Clear();var snapshot=state?["graphics"];
            dialog.Height=Math.Clamp(216+(snapshot?["options"]?.AsArray().Count??0)*48,360,dialog.MaxHeight);
            foreach(var button in presetButtons)button.IsEnabled=B(snapshot,"available")&&!B(state,"gameRunning");
            if(!B(snapshot,"available")){note.Text=S(snapshot,"reason");if(save!=null)save.IsEnabled=false;return;}
            foreach(var item in snapshot?["options"]?.AsArray()??new())
            {
                if(item==null)continue;string name=S(item,"name");JsonNode? original=item["value"]?.DeepClone();
                var row=new Grid{ColumnDefinitions=new ColumnDefinitions("*,230"),ColumnSpacing=16};
                var label=new StackPanel{Spacing=2,VerticalAlignment=VerticalAlignment.Center};label.Children.Add(Text(S(item,"label"),13,"#DCEAF1"));
                if(B(item,"requiresRestart"))label.Children.Add(Text("В игре требует перезапуска",11,"#89AFC5"));
                else if(B(item,"menuOnly"))label.Children.Add(Text("В игре доступно из главного меню",11,"#89AFC5"));
                row.Children.Add(label);
                Control editor;
                if(!B(item,"editable")){editor=Text(GraphicsValueLabel(original?.ToString()??""),12,"#8299A8");ToolTip.SetTip(editor,"Этот динамический параметр выбирается в игре.");}
                else if(name=="FrameGeneration" && item["choices"] is JsonArray fgChoices && fgChoices.Count==2 && fgChoices[0]?.ToString()=="Off" && fgChoices[1]?.ToString()=="DLSS")
                {
                    bool value=original?.ToString()=="DLSS";bool actual=value;
                    var binding=new FeatureToggleBinding(()=>new FeatureToggleState(value?"DLSS · Вкл":"DLSS · Выкл",null,value,actual,true),v=>{value=v;Pending();});
                    var button=new FeatureToggleButton(binding,()=>busy||B(state,"gameRunning"),"Генерация кадров в настройках Cyberpunk. Сохрани перед запуском игры.","");
                    button.Name="GameFrameGeneration";
                    fields[name]=(original,()=>JsonValue.Create(value?"DLSS":"Off"),v=>{value=v?.ToString()=="DLSS";button.Refresh();});editor=button;
                }
                else if(S(item,"type")=="bool")
                {
                    bool invert=B(item,"invertDisplay");bool value=(original?.GetValue<bool>()??false)^invert;bool actual=value;
                    var binding=new FeatureToggleBinding(()=>new FeatureToggleState(value?"Вкл":"Выкл",null,value,actual,true),v=>{value=v;Pending();});
                    var button=new FeatureToggleButton(binding,()=>busy||B(state,"gameRunning"),"Изменение будет записано после сохранения.","");
                    fields[name]=(original,()=>JsonValue.Create(value^invert),v=>{value=(v?.GetValue<bool>()??false)^invert;button.Refresh();});editor=button;
                }
                else if((item["choices"]?.AsArray().Count??0)>0)
                {
                    var choices=item["choices"]!.AsArray().Select(v=>new GraphicsChoice(v?.DeepClone(),GraphicsValueLabel(v?.ToString()??""))).ToArray();
                    var box=new ComboBox{ItemsSource=choices,SelectedItem=choices.FirstOrDefault(c=>JsonNode.DeepEquals(c.Value,original)),MinHeight=30,FontSize=12,HorizontalAlignment=HorizontalAlignment.Stretch};
                    fields[name]=(original,()=>((box.SelectedItem as GraphicsChoice)?.Value??original)?.DeepClone(),v=>box.SelectedItem=choices.FirstOrDefault(c=>JsonNode.DeepEquals(c.Value,v)));
                    box.SelectionChanged+=(_,_)=>Pending();editor=box;
                }
                else
                {
                    var number=new NumericUpDown{Value=Number(original),Minimum=Number(item["minimum"]),Maximum=Number(item["maximum"]),Increment=Number(item["step"],1),FormatString=S(item,"type")=="int"?"0":"0.00",MinHeight=30,FontSize=12};
                    fields[name]=(original,()=>JsonValue.Create(number.Value??0),v=>number.Value=Number(v));number.ValueChanged+=(_,_)=>Pending();editor=number;
                }
                editor.IsEnabled=!B(state,"gameRunning");Grid.SetColumn(editor,1);row.Children.Add(editor);form.Children.Add(row);
            }
            Pending();
        }
        var presets=new StackPanel{Orientation=Orientation.Horizontal,Spacing=7};
        foreach(var entry in new[]{("performance","Скорость"),("balanced","Баланс"),("quality","Качество")})
        {
            var key=entry.Item1;
            var preset=Btn(entry.Item2,()=>{
                if(state?["graphicsPresets"]?[key] is JsonObject values){foreach(var pair in values){if(fields.TryGetValue(pair.Key,out var field))field.set(pair.Value);}Pending();}
            },"AppleSecondaryBtn","Пресет DLSS, текстур и плотности толпы. Лучи выключены; генерация кадров не включается.");
            preset.IsEnabled=B(state?["graphics"],"available")&&!B(state,"gameRunning");presetButtons.Add(preset);presets.Children.Add(preset);
        }
        var refresh=Btn("Обновить",async()=>{await Run("ReadGraphicsSettings");if(error)note.Text=result;else Populate();});
        save=Btn("Сохранить",async()=>{
            var changes=Changes();if(changes.Count==0||busy)return;
            await Run("ApplyGraphicsSettings",new JsonObject{["hash"]=S(state?["graphics"],"hash"),["values"]=changes});
            if(error){note.Text=result;note.Foreground=Brush("#FFB4AA");}else{Populate();note.Text="Настройки сохранены.";}
        },"ApplePrimaryBtn");
        var actions=new StackPanel{Orientation=Orientation.Horizontal,Spacing=8,Children={save,refresh,Btn("Закрыть",dialog.Close)}};
        var layout=new Grid{Margin=new Thickness(22),RowDefinitions=new RowDefinitions("Auto,*,Auto,Auto,Auto"),RowSpacing=14};
        layout.Children.Add(Text("Настройки игры",21,"#ECF5FA"));Grid.SetRow(presets,2);layout.Children.Add(presets);
        var scroll=new ScrollViewer{Content=form,HorizontalScrollBarVisibility=Avalonia.Controls.Primitives.ScrollBarVisibility.Disabled};Grid.SetRow(scroll,1);layout.Children.Add(scroll);
        Grid.SetRow(note,3);layout.Children.Add(note);Grid.SetRow(actions,4);layout.Children.Add(actions);
        Populate();dialog.Content=layout;dialog.Closing+=(_,e)=>{if(busy)e.Cancel=true;};await dialog.ShowDialog(this);
    }
}

using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Threading;
using System.Diagnostics;
using System.Reflection;
using System.Text.Json;
using VRPorts;

static class UiPreview
{
    public static void Run(string output)
    {
        output = Path.GetFullPath(output); Directory.CreateDirectory(output);
        string fixture = Path.Combine(output, "fixture"); Directory.CreateDirectory(fixture);
        string pwsh = Environment.GetEnvironmentVariable("VRPORTS_TEST_PWSH") ?? "pwsh";
        object Port(string id, string name, string family, string kind, bool installed = false) => new {
            id, label=name, family, kind, installed, available=true, allowOpti=true, allowFG=true, handsAvailable=true,
            sourceRepoId=family=="iPowerTech"?"ipower":family=="Ajson"?"ajson":"", note="Демонстрационные данные интерфейса",
            addedUtc="2026-10-12T12:00:00Z", builtUtc="2026-10-12T12:00:00Z", recommendedSettings=false
        };
        object Option(string name,string label,string type,object value,string[]? choices=null,decimal? min=null,decimal? max=null,decimal? step=null) => new {name,label,type,value,choices=choices??[],minimum=min,maximum=max,step,editable=true,requiresRestart=name=="FrameGeneration",menuOnly=name=="TextureQuality"};
        var state = new {
            game=Path.Combine(fixture,"game"), exePresent=true, hold=false, gameRunning=false, vortexRunning=false,
            receipt=(object?)null,
            versions=new[]{Port("flat","Плоская игра","Плоская игра","flat",true),Port("tofu-aer-v1-candidate","Astra HD · текущая сборка","Astra HD","native"),Port("luke-27","Luke Ross · 27","Luke Ross","luke"),Port("ipower-latest","iPowerTech · оригинал","iPowerTech","native"),Port("ajson-v47","Ajson · V47","Ajson","native")},
            libraries=new object[]{new {file="nvngx_dlss.dll",label="DLSS Super Resolution",current="sr",version="310.4",choices=new[]{new {hash="sr",label="Установленная версия"}}}},
            components=Array.Empty<object>(), modsEnabled=true, activeExtraMods=Array.Empty<object>(),parkedMods=Array.Empty<object>(),
            recommendedSettings=false, nrEnabled=false, usbRecovery=new {enabled=true,available=true},keyBindings=new {},
            graphics=new {available=true,hash=new string('a',64),options=new[]{Option("FrameGeneration","Генерация кадров","string_list","Off",["Off","DLSS"]),Option("DLSS_MultiFrameGeneration","Множитель кадров DLSS","string_list","x2",["x2","x3","x4"]),Option("ResolutionScaling","Масштабирование","string_list","DLSS",["Off","DLSS","FSR","XeSS"]),Option("DLSS","Режим DLSS","string_list","Balanced",["Auto","DLAA","Quality","Balanced","Performance","Ultra Performance"]),Option("DLSS_NewSharpness","Резкость DLSS","float",0.3m,null,0,1,0.05m),Option("TextureQuality","Текстуры","string_list","High",["Low","Medium","High"]),Option("RayTracing","Трассировка лучей","bool",true),Option("RayTracedPathTracing","Path Tracing","bool",false),Option("CrowdDensity","Плотность толпы","string_list","Medium",["Low","Medium","High"])}},
            graphicsPresets=new {performance=new {DLSS="Performance",TextureQuality="Medium",RayTracing=false},balanced=new {DLSS="Balanced",TextureQuality="High",RayTracing=false},quality=new {DLSS="Quality",TextureQuality="High",RayTracing=false}},
            collection=new {kind="vortex",profile="stable-super-ai-id",profileId="stable-super-ai-id",profileName="SuperCyberpunkAI", importedUtc="2026-10-16T12:00:00Z",origin=new {root=Path.Combine(fixture,"vortex"),profileId="stable-super-ai-id"}},
            redmod=new {enabled=true,installed=true,available=true}, enhancementPackage=new {enabled=true,compatible=true,available=true,status="NR + MFG подключён",sourceLabel="OptiScaler 10 + NR + MFG"}, testSaves=new {enabled=false,selectedIds=new[]{"story","editor","inventory","mirror","city","test"},installedCount=0,libraryRoot=Path.Combine(fixture,"test-save-library"),sets=new[]{
                new {id="story",label="Сюжетные сохранения",saveCount=9,selected=true},new {id="editor",label="Редактор персонажа",saveCount=1,selected=true},
                new {id="inventory",label="Инвентарь",saveCount=1,selected=true},new {id="mirror",label="У зеркала",saveCount=1,selected=true},
                new {id="city",label="В городе",saveCount=1,selected=true},new {id="test",label="Проверка",saveCount=1,selected=true}
            }},
            gameProfiles=new {enabled=true,available=true,initialized=true,activeId="current",profiles=new[]{new {id="current",name="Super AI",vr=true,createdUtc="2026-10-12T10:00:00Z"},new {id=new string('b',32),name="Чистая игра",vr=false,createdUtc="2026-10-12T09:00:00Z"}},recoveryRequired=Array.Empty<string>()}
        };
        File.WriteAllText(Path.Combine(fixture,"state.json"),JsonSerializer.Serialize(state));
        File.WriteAllText(Path.Combine(fixture,"config.json"),JsonSerializer.Serialize(new {game=state.game,data=Path.Combine(fixture,"data"),pwsh,gameProfilesEnabled=true}));
        File.WriteAllText(Path.Combine(fixture,"Backend.ps1"),"""
param([string]$Request,[string]$Result,[string]$ConfigPath)
$req=Get-Content -LiteralPath $Request -Raw|ConvertFrom-Json
$state=Get-Content -LiteralPath "$PSScriptRoot/state.json" -Raw|ConvertFrom-Json
@{ok=$true;action=$req.action;state=$state;output='Демонстрационные данные';focusVersionId=''}|ConvertTo-Json -Depth 20|Set-Content -LiteralPath $Result -Encoding utf8
""");
        var rootProperty=typeof(MainWindow).Assembly.GetType("VRPorts.Program")!.GetProperty("Root",BindingFlags.Public|BindingFlags.Static)!;
        var previous=rootProperty.GetValue(null); MainWindow? window=null;
        try
        {
            rootProperty.SetValue(null,fixture);window=new MainWindow();window.Show();
            var watch=Stopwatch.StartNew();
            while(watch.Elapsed<TimeSpan.FromSeconds(20))
            {
                Dispatcher.UIThread.RunJobs();window.UpdateLayout();Thread.Sleep(20);
                bool busy=(bool)typeof(MainWindow).GetField("busy",BindingFlags.Instance|BindingFlags.NonPublic)!.GetValue(window)!;
                if(!busy&&watch.ElapsedMilliseconds>500)break;
            }
            typeof(MainWindow).GetField("result",BindingFlags.Instance|BindingFlags.NonPublic)!.SetValue(window,"Предпросмотр · демонстрационные данные");
            typeof(MainWindow).GetMethod("BuildLibrary",BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(window,null);
            void Capture(string name,Window? target=null)
            {
                target??=window;target.InvalidateVisual();Dispatcher.UIThread.RunJobs();target.UpdateLayout();Thread.Sleep(180);
                AvaloniaHeadlessPlatform.ForceRenderTimerTick();
                using var bitmap=target.CaptureRenderedFrame()??throw new Exception("No rendered frame.");
                bitmap.Save(Path.Combine(output,name+".png"));
            }
            Capture("home");
            typeof(MainWindow).GetField("showPorts",BindingFlags.Instance|BindingFlags.NonPublic)!.SetValue(window,true);
            typeof(MainWindow).GetMethod("BuildLibrary",BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(window,null);
            Capture("ports");
            typeof(MainWindow).GetMethod("OpenManage",BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(window,null);
            Capture("settings");
            typeof(MainWindow).GetField("advanced",BindingFlags.Instance|BindingFlags.NonPublic)!.SetValue(window,true);
            typeof(MainWindow).GetMethod("BuildManage",BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(window,null);
            Capture("advanced");
            var graphicTask=(Task)typeof(MainWindow).GetMethod("GraphicsSettingsDialog",BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(window,null)!;
            Window? graphics=null;watch.Restart();
            while(watch.Elapsed<TimeSpan.FromSeconds(20)){
                Dispatcher.UIThread.RunJobs();Thread.Sleep(20);
                var owned=(System.Collections.IEnumerable)typeof(Window).GetProperty("OwnedWindows")!.GetValue(window)!;
                graphics=owned.Cast<Window>().FirstOrDefault(w=>w.IsVisible);if(graphics!=null)break;
            }
            if(graphics==null)throw new Exception("Graphics dialog did not open.");
            Capture("graphics",graphics);graphics.Close();Dispatcher.UIThread.RunJobs();graphicTask.GetAwaiter().GetResult();
            Console.WriteLine("Rendered actual Avalonia UI with synthetic data: "+output);
        }
        finally {window?.Close();rootProperty.SetValue(null,previous);}
    }
}

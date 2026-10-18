#include "Overlay/VrWidgets.hpp"
#include "Overlay/LiveControlsUi.hpp"
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace overlay::widgets {
namespace {
const ImVec4 Red{1,.28f,.29f,1}, Cyan{.30f,.88f,.94f,1}, Yellow{.98f,.94f,.06f,1};
void Outline(ImDrawList* draw,ImVec2 a,ImVec2 b,ImU32 color,bool fill=false) {
    const float cut=std::min(10.0f,(b.y-a.y)*.22f);
    const ImVec2 points[]={{a.x,a.y},{b.x,a.y},{b.x,b.y-cut},{b.x-cut,b.y},{a.x,b.y}};
    if(fill)draw->AddConvexPolyFilled(points,5,color);
    else draw->AddPolyline(points,5,color,ImDrawFlags_Closed,1.2f);
}
void CenterText(ImDrawList* draw,ImVec2 a,ImVec2 b,const char* text,ImU32 color) {
    const auto size=ImGui::CalcTextSize(text);
    const float fit=std::min(1.f,std::max(1.f,b.x-a.x-12)/std::max(1.f,size.x));
    draw->PushClipRect(a,b,true);
    draw->AddText(ImGui::GetFont(),ImGui::GetFontSize()*fit,
        {a.x+std::max(4.0f,(b.x-a.x-size.x*fit)*.5f),a.y+(b.y-a.y-size.y*fit)*.5f},color,text);
    draw->PopClipRect();
}
struct Field {
    ImVec2 a,b;
    ImDrawList* draw;
    float height,arrow,width;
    explicit Field(const char* label) {
        ImGui::BeginGroup();ImGui::PushID(label);
        const ImVec2 start=ImGui::GetCursorScreenPos();
        const float available=std::max(160.0f,ImGui::GetContentRegionAvail().x);
        const char* end=ImGui::FindRenderedTextEnd(label);
        const bool named=end!=label;
        const float labelWidth=named?available*.40f:0;
        const float gap=named?ImGui::GetStyle().ItemInnerSpacing.x:0;
        const auto size=named?ImGui::CalcTextSize(label,end,false,std::max(1.0f,labelWidth-8)):ImVec2{};
        height=std::max(44.0f,ImGui::GetFrameHeight());
        const float rowHeight=std::max(height,size.y+8);
        draw=ImGui::GetWindowDrawList();
        if(named)draw->AddText(ImGui::GetFont(),ImGui::GetFontSize(),{start.x,start.y+(rowHeight-size.y)*.5f},
            ImGui::GetColorU32(Red),label,end,labelWidth-8);
        ImGui::Dummy({labelWidth,rowHeight});
        a={start.x+labelWidth+gap,start.y+(rowHeight-height)*.5f};
        width=std::max(100.0f,available-labelWidth-gap);b={a.x+width,a.y+height};
        arrow=std::min(height,width*.18f);
        Outline(draw,a,b,ImGui::GetColorU32(ImVec4(.065f,.025f,.040f,.9f)),true);
        Outline(draw,a,b,ImGui::GetColorU32(ImVec4(.62f,.18f,.21f,1)));
    }
    ~Field(){ImGui::PopID();ImGui::EndGroup();}
    bool Arrow(bool right,bool enabled=true) {
        const ImVec2 p{right?b.x-arrow:a.x,a.y};
        ImGui::SetCursorScreenPos(p);
        ImGui::PushStyleColor(ImGuiCol_Button,{0,0,0,0});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,{.10f,.24f,.27f,.4f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,{.16f,.32f,.34f,.7f});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,0);
        ImGui::BeginDisabled(!enabled);ImGui::PushButtonRepeat(true);
        const bool clicked=ImGui::Button(right?"##next":"##previous",{arrow,height});
        ImGui::PopButtonRepeat();
        const ImU32 color=ImGui::GetColorU32(ImGui::IsItemActive()?Yellow:Cyan);
        const float x=p.x+arrow*.5f,y=p.y+height*.5f,s=std::min(9.0f,height*.16f),sign=right?1.f:-1.f;
        draw->AddTriangle({x+sign*s,y},{x-sign*s,y-s},{x-sign*s,y+s},color,1.4f);
        ImGui::EndDisabled();ImGui::PopStyleVar();ImGui::PopStyleColor(3);
        return clicked;
    }
    ImVec2 TrackMin() const{return {a.x+arrow,a.y};}
    ImVec2 TrackMax() const{return {b.x-arrow,b.y};}
};
template<class T> bool Slider(const char* label,T* value,T minimum,T maximum,const char* format,ImGuiSliderFlags flags,ImGuiDataType type) {
    Field field(label);
    const int precision=type==ImGuiDataType_S32?0:std::clamp(ImParseFormatPrecision(format,3),0,6);
    const T step=type==ImGuiDataType_S32?T(1):T(std::pow(10.0,-precision));
    bool changed=false;
    if(field.Arrow(false,*value>minimum)) {const T next=std::clamp(T(*value-step),minimum,maximum);changed=next!=*value;*value=next;}
    const auto a=field.TrackMin(),b=field.TrackMax();
    char text[96]{},low[96]{},high[96]{};
    ImGui::DataTypeFormatString(low,sizeof(low),type,&minimum,format);
    ImGui::DataTypeFormatString(high,sizeof(high),type,&maximum,format);
    const float grab=std::min(std::max(ImGui::CalcTextSize(low).x,ImGui::CalcTextSize(high).x)+24,std::max(24.f,b.x-a.x-4));
    field.draw->AddLine({a.x+2,(a.y+b.y)*.5f},{b.x-2,(a.y+b.y)*.5f},ImGui::GetColorU32(ImVec4(.44f,.12f,.16f,1)),2);
    ImGui::SetCursorScreenPos(a);ImGui::SetNextItemWidth(b.x-a.x);
    const auto id=ImGui::GetID("##value");
    const bool editing=ImGui::TempInputIsActive(id) || (ImGui::GetIO().KeyCtrl && ImGui::IsMouseHoveringRect(a,b));
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize,grab);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,0);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(4,(field.height-ImGui::GetFontSize())*.5f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,{0,0,0,0});
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,{0,0,0,0});
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,{0,0,0,0});
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,{0,0,0,0});
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive,{0,0,0,0});
    ImGui::PushStyleColor(ImGuiCol_Text,editing?Cyan:ImVec4(0,0,0,0));
    changed|=ImGui::SliderScalar("##value",type,value,&minimum,&maximum,format,flags|ImGuiSliderFlags_AlwaysClamp);
    const bool hot=ImGui::IsItemHovered() || ImGui::IsItemActive();
    ImGui::PopStyleColor(6);ImGui::PopStyleVar(3);
    if(!ImGui::TempInputIsActive(id)) {
        const float ratio=maximum>minimum?std::clamp(float(double(*value-minimum)/double(maximum-minimum)),0.f,1.f):0;
        const float usable=std::max(0.f,b.x-a.x-4-grab);
        const ImVec2 left{a.x+2+usable*ratio,a.y+3},right{left.x+grab,b.y-3};
        Outline(field.draw,left,right,ImGui::GetColorU32(ImVec4(.15f,.035f,.05f,1)),true);
        Outline(field.draw,left,right,ImGui::GetColorU32(hot?Yellow:Red));
        ImGui::DataTypeFormatString(text,sizeof(text),type,value,format);
        CenterText(field.draw,left,right,text,ImGui::GetColorU32(Cyan));
    }
    if(field.Arrow(true,*value<maximum)) {const T next=std::clamp(T(*value+step),minimum,maximum);changed|=next!=*value;*value=next;}
    return changed;
}
}
bool SliderFloat(const char* label,float* v,float lo,float hi,const char* format,ImGuiSliderFlags flags){return Slider(label,v,lo,hi,format,flags,ImGuiDataType_Float);}
bool SliderInt(const char* label,int* v,int lo,int hi,const char* format,ImGuiSliderFlags flags){return Slider(label,v,lo,hi,format,flags,ImGuiDataType_S32);}
bool Combo(const char* label,int* selected,const char* const* items,int count,int) {
    Field field(label);bool changed=false;
    if(field.Arrow(false,count>0 && *selected>0)){*selected=std::clamp(*selected-1,0,count-1);changed=true;}
    const auto a=field.TrackMin(),b=field.TrackMax();
    ImGui::SetCursorScreenPos(a);ImGui::SetNextItemWidth(b.x-a.x);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,0);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(10,(field.height-ImGui::GetFontSize())*.5f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,{0,0,0,0});
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,{.10f,.24f,.27f,.25f});
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,{.10f,.24f,.27f,.35f});
    const bool open=ImGui::BeginCombo("##list","",ImGuiComboFlags_NoArrowButton|ImGuiComboFlags_HeightLarge);
    ImGui::PopStyleColor(3);ImGui::PopStyleVar(2);
    if(open) {
        for(int i=0;i<count;++i) {
            ImGui::PushID(i);ImGui::PushStyleColor(ImGuiCol_Text,i==*selected?Yellow:Cyan);
            if(ImGui::Selectable(items[i],i==*selected,0,{0,std::max(44.f,ImGui::GetFrameHeight())})) {*selected=i;changed=true;}
            if(i==*selected)ImGui::SetItemDefaultFocus();
            ImGui::PopStyleColor();ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    CenterText(field.draw,a,b,*selected>=0 && *selected<count?items[*selected]:"--",ImGui::GetColorU32(Cyan));
    if(field.Arrow(true,count>0 && *selected<count-1)){*selected=std::clamp(*selected+1,0,count-1);changed=true;}
    return changed;
}
bool Combo(const char* label,int* selected,const char* list,int height) {
    std::vector<const char*> items;
    for(const char* p=list;*p;p+=std::strlen(p)+1)items.push_back(p);
    return Combo(label,selected,items.data(),int(items.size()),height);
}
void Section(const char* label) {
    ImGui::Spacing();ImGui::TextColored(Cyan,"%s",label);
    const auto p=ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(p,{p.x+ImGui::GetContentRegionAvail().x,p.y},ImGui::GetColorU32(ImVec4(.25f,.72f,.80f,.65f)));
    ImGui::Dummy({0,5});
}
void Tabs(const char* id,int& selected,const char* const* labels,int count) {
    ImGui::PushID(id);const float width=(ImGui::GetContentRegionAvail().x-ImGui::GetStyle().ItemSpacing.x*(count-1))/count;
    for(int i=0;i<count;++i) {
        if(i)ImGui::SameLine();ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Text,selected==i?Yellow:Cyan);
        if(ImGui::Button(labels[i],{width,std::max(46.f,ImGui::GetFrameHeight())}))selected=i;
        if(selected==i){const auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();ImGui::GetWindowDrawList()->AddLine({a.x,b.y},{b.x,b.y},ImGui::GetColorU32(Yellow),2);}
        ImGui::PopStyleColor();ImGui::PopID();
    }
    ImGui::PopID();ImGui::Spacing();
}
void DrawBindings(const LiveControlsUiState& state) {
    struct Binding { const char* input;const char* action; };
    auto group=[](const char* title,const Binding* rows,int count) {
        Section(title);
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,ImVec2(12,12));
        if(ImGui::BeginTable(title,2,ImGuiTableFlags_SizingStretchProp|ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("ВВОД",ImGuiTableColumnFlags_WidthStretch,.36f);
            ImGui::TableSetupColumn("ДЕЙСТВИЕ",ImGuiTableColumnFlags_WidthStretch,.64f);
            for(int i=0;i<count;++i) {
                ImGui::TableNextRow(0,44);ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text,Cyan);ImGui::TextWrapped("%s",rows[i].input);ImGui::PopStyleColor();
                ImGui::TableNextColumn();ImGui::TextWrapped("%s",rows[i].action);
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
    };
    const int configuredMode=std::clamp(state.xrChordActivation,0,2);
    const bool extras=state.xrExtraChordActions!=0;
    const int effectiveMode=(configuredMode==2 && !state.xrRightThumbrestAvailable)?0:configuredMode;
    const char* modifier=effectiveMode==0?"L3":effectiveMode==1?"R3":"ПРАВАЯ ПЛОЩАДКА ДЛЯ ПАЛЬЦА";
    const char* dpadStick=effectiveMode==0?"ПРАВЫЙ СТИК":"ЛЕВЫЙ СТИК";
    const char* recenterButton=effectiveMode==0?"A":"X";
    const char* overlayButton=effectiveMode==0?"B":"Y";
    char dpadInput[96]{},backInput[96]{},releaseInput[96]{},releaseAction[128]{};
    char recenterInput[96]{},overlayInput[96]{},cyberwareInput[96]{};
    std::snprintf(dpadInput,sizeof(dpadInput),"%s + %s",modifier,dpadStick);
    std::snprintf(backInput,sizeof(backInput),"%s + ЛЕВАЯ КНОПКА МЕНЮ",modifier);
    std::snprintf(recenterInput,sizeof(recenterInput),"%s + %s",modifier,recenterButton);
    std::snprintf(overlayInput,sizeof(overlayInput),"%s + %s",modifier,overlayButton);
    std::snprintf(cyberwareInput,sizeof(cyberwareInput),"%s + ЛЕВАЯ КНОПКА ЗАХВАТА",modifier);

    ImGui::TextWrapped("Сочетания кнопок VR и стандартные игровые действия. Названия кнопок соответствуют Touch / Xbox; игровые действия зависят от ваших назначений кнопок контроллера.");
    static const Binding foot[]{
        {"ЛЕВЫЙ СТИК","Движение. Для бега удерживайте стик до упора вперёд 0,2 секунды."},
        {"ПРАВЫЙ СТИК  Влево / вправо","Поворот с учётом настройки ступенчатого поворота."},
        {"ПРАВЫЙ СТИК  До упора вверх","Рывок / уклонение один раз за отклонение стика."},
        {"ПРАВЫЙ СТИК  До упора вниз","Присесть."},
        {"ПРАВЫЙ СТИК  Нажать","Передёрнуть затвор / снять с затворной задержки."},
        {"A  Правый контроллер","Прыжок."},
        {"B  Правый контроллер","С оружием в руке: извлечь магазин. С убранным оружием: назад / закрыть."},
        {"X  Левый контроллер","Взаимодействие / перезарядка."},
        {"Y  Левый контроллер","Сменить оружие."},
        {"ПРАВЫЙ КУРОК","Стрельба."},
        {"ЛЕВЫЙ КУРОК","Прицелиться / блок в ближнем бою."},
        {"ПРАВАЯ КНОПКА ЗАХВАТА  У кобуры","Взять или убрать оружие в слот, к которому поднесена рука."},
        {"ЛЕВАЯ КНОПКА ЗАХВАТА","Взять магазин во время перезарядки."},
        {"ЛЕВАЯ КНОПКА ЗАХВАТА  У левого уха","Включить сканер. Нажмите ещё раз, чтобы закрыть его."},
        {"ЛЕВАЯ КНОПКА МЕНЮ","Открыть меню паузы игры."}
    };
    Binding footClassic[]{
        {"ЛЕВЫЙ СТИК",state.xrClassicDisableLsSprint?"Движение. Включение бега отклонением стика до упора вперёд отключено.":"Движение. Для бега удерживайте стик до упора вперёд 0,2 секунды."},
        {"ПРАВЫЙ СТИК  Влево / вправо","Поворот с учётом настройки ступенчатого поворота."},
        {"ПРАВЫЙ СТИК  Вверх / вниз",state.xrClassicDisableRsDashCrouch?"Обычный ввод по вертикальной оси правого стика; жесты рывка и приседания отключены.":"Обычный ввод по вертикальной оси правого стика; до упора вверх - рывок / уклонение, до упора вниз - присесть."},
        {"A / B / X / Y","Обычные назначения лицевых кнопок Xbox."},
        {"НАЖАТИЕ ЛЕВОГО / ПРАВОГО СТИКА","Обычные назначения L3 / R3 на Xbox."},
        {"ЛЕВЫЙ / ПРАВЫЙ КУРОК","Обычные назначения LT / RT на Xbox."},
        {"ЛЕВАЯ / ПРАВАЯ КНОПКА ЗАХВАТА","LB / RB, если эту кнопку захвата не использует текущее взаимодействие в VR."},
        {"ПРАВАЯ КНОПКА ЗАХВАТА  У кобуры","Взять или убрать оружие, к которому поднесена рука; это нажатие заменяет RB."},
        {"ЛЕВАЯ КНОПКА ЗАХВАТА  У левого уха","Включить сканер. Нажмите ещё раз, чтобы закрыть его."},
        {"ЛЕВАЯ КНОПКА МЕНЮ","Открыть меню паузы игры."}
    };
    if(state.xrClassicOnFootControls) group("ПЕШКОМ",footClassic,IM_ARRAYSIZE(footClassic));
    else group("ПЕШКОМ",foot,IM_ARRAYSIZE(foot));
    static const Binding swimming[]{
        {"ОБЕ РУКИ  Вытянуть, развести, подтянуть","Плыть в направлении взгляда. Движение начинается при подтягивании рук; плавные гребки дают обычную скорость, быстрые - ускорение."},
        {"ОБЕ РУКИ  Поднять, затем опустить","Плыть вверх."},
        {"ЛЕВЫЙ СТИК  Назад","Отменить движение гребками. Движение стиком имеет приоритет."}
    };
    group("ПЛАВАНИЕ",swimming,IM_ARRAYSIZE(swimming));
    static const Binding scanner[]{
        {"ЛЕВЫЙ СТИК  До упора вверх / вниз","Листать быстрые взломы. Неполное отклонение стика по-прежнему отвечает за ходьбу."},
        {"X  Левый контроллер","Применить выбранный быстрый взлом."},
        {"ПРАВЫЙ КУРОК","Отметить цель при сканировании."},
        {"ПРАВЫЙ СТИК  Нажать","Сменить вкладку сканера."},
        {"ЛЕВЫЙ КУРОК + ПРАВЫЙ СТИК","Настроить увеличение сканера."}
    };
    static const Binding scannerClassic[]{
        {"РУКА У ЛЕВОГО УХА + ЛЕВАЯ КНОПКА ЗАХВАТА","Включить / выключить сканер; пока он включён, LB остаётся нажатой программно."},
        {"СТИКИ / ЛИЦЕВЫЕ КНОПКИ","Обычные назначения Xbox при включённом сканере."},
        {"ЛЕВЫЙ / ПРАВЫЙ КУРОК","Обычные назначения LT / RT на Xbox."},
        {"ЛЕВАЯ / ПРАВАЯ КНОПКА ЗАХВАТА","LB / RB, если эту кнопку захвата не использует текущее взаимодействие в VR."}
    };
    if(state.xrClassicScannerControls) group("СКАНЕР",scannerClassic,IM_ARRAYSIZE(scannerClassic));
    else group("СКАНЕР",scanner,IM_ARRAYSIZE(scanner));
    Binding dpad[3]{
        {dpadInput,"Удерживая кнопку-модификатор, сначала верните стик в центр, затем отклоните до упора вверх, вниз, влево или вправо, чтобы нажать соответствующее направление крестовины."},
        {backInput,"Передать Back / Select вместо Start."},
        {nullptr,nullptr}
    };
    int dpadCount=2;
    if(effectiveMode<2){
        std::snprintf(releaseInput,sizeof(releaseInput),"Отпустить %s без сочетания",modifier);
        std::snprintf(releaseAction,sizeof(releaseAction),"Обычное действие нажатия стика %s.",modifier);
        dpad[dpadCount++]={releaseInput,releaseAction};
    }
    group("СОЧЕТАНИЯ КНОПОК",dpad,dpadCount);

    const char* disabled="Недоступно, пока выключены «Дополнительные сочетания».";
    Binding extra[]{
        {recenterInput,extras?"Сбросить центр отслеживания VR.":disabled},
        {overlayInput,extras?"Открыть или закрыть панель настроек VR.":disabled},
        {cyberwareInput,extras?"Активировать культовый имплант один раз за нажатие кнопки захвата; ограничения игровой ситуации и время восстановления сохраняются.":disabled}
    };
    group("ДОПОЛНИТЕЛЬНЫЕ СОЧЕТАНИЯ",extra,IM_ARRAYSIZE(extra));
    static const Binding driving[]{
        {"Удерживать X  Левый контроллер","Выйти из транспорта."},
        {"A  Правый контроллер","Подтвердить вариант ответа. Обычное действие ручного тормоза остаётся доступным."},
        {"ЛЕВЫЙ / ПРАВЫЙ КУРОК","Тормоз / газ."},
        {"КНОПКА ЗАХВАТА  У руля","Взяться этой рукой за руль. Отпустите кнопку, чтобы отпустить руль."},
        {"ИГРОВАЯ КАМЕРА","VR следует за выбранной игрой камерой, включая сцены от третьего лица. Настройки вождения и камеры сидя находятся в разделе «ВОЖДЕНИЕ»."}
    };
    Binding drivingClassic[]{
        {"A / B / X / Y","Обычные назначения лицевых кнопок Xbox в транспорте; без замены A на X и выхода по удержанию X."},
        {"ЛЕВЫЙ / ПРАВЫЙ СТИК","Обычные назначения стиков Xbox в транспорте."},
        {"НАЖАТИЕ ЛЕВОГО / ПРАВОГО СТИКА","Обычные назначения L3 / R3 на Xbox."},
        {"ЛЕВЫЙ / ПРАВЫЙ КУРОК",state.xrClassicSwapTriggersGrips?"LB / RB.":"Обычные назначения LT / RT на Xbox."},
        {"ЛЕВАЯ / ПРАВАЯ КНОПКА ЗАХВАТА",state.xrClassicSwapTriggersGrips?"Аналоговые LT / RT для тормоза и газа.":"LB / RB."},
        {"ЗАХВАТ РУЛЯ","Недоступно при классическом управлении транспортом."},
        {"ИГРОВАЯ КАМЕРА","VR следует за выбранной игрой камерой, включая сцены от третьего лица. Настройки камеры сидя находятся в разделе «ВОЖДЕНИЕ»."}
    };
    if(state.xrClassicVehicleControls) group("ВОЖДЕНИЕ",drivingClassic,IM_ARRAYSIZE(drivingClassic));
    else group("ВОЖДЕНИЕ",driving,IM_ARRAYSIZE(driving));
    static const Binding overlay[]{
        {"Навести + КУРОК","Выбрать элемент."},
        {"СТИК  Вверх / вниз","Прокрутить панель под указателем."},
        {"КНОПКА ЗАХВАТА","Перетащить панель."},
        {"КНОПКА ЗАХВАТА + СТИК  Вверх / вниз","Отдалить / приблизить панель."},
        {"F10 / INSERT","Открыть / закрыть панель с клавиатуры. ESC закрывает панель."}
    };
    group("ПАНЕЛЬ",overlay,IM_ARRAYSIZE(overlay));
    ImGui::Spacing();ImGui::TextWrapped("Назначения игровых действий меняются в «Настройки > Назначение клавиш > Контроллер». Настройки жестов VR находятся в разделах «УПРАВЛЕНИЕ» и «АВАТАР».");
}
}

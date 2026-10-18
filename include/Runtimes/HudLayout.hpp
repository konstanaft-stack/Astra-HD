#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace cvr::hud {
struct ElementDefinition { const char* name; const char* label; const char* key; };
inline constexpr ElementDefinition Elements[] = {
    {"player_health_bar","Здоровье","health"}, {"activity_log","Журнал событий","activity"},
    {"bottom_hints","Подсказки кнопок","hints"}, {"ammo_counter","Оружие / боеприпасы","ammo"},
    {"warning","Предупреждение","warning"}, {"cursor_device","Указатель устройства","device_cursor"},
    {"dpad_hint","Быстрые слоты","quick_slots"}, {"boss_healthbar","Здоровье босса","boss"},
    {"hud_progress_bar","Шкала прогресса","progress"}, {"oxygenbar","Кислород","oxygen"},
    {"minimap","Мини-карта","minimap"}, {"wanted_bar","Уровень розыска","wanted"},
    {"minimap_q305","Альтернативная мини-карта","minimap_alternate"},
    {"vehicle_summon_notification","Вызов транспорта","vehicle_summon"},
    {"quest_list","Отслеживание задания","quest"}, {"vehicle scan widget","Сканирование транспорта","vehicle_scan"},
    {"cooldown","Восстановление","cooldown"}, {"input_hint","Подсказка ввода","input_prompt"},
    {"zone alert notification","Уведомление о зоне","zone_alert"}, {"staminabar","Выносливость","stamina"},
    {"crouch_indicator","Индикатор приседания","crouch"}, {"militech warning","Предупреждение «Милитеха»","militech"},
    {"new_phone","Телефон / сообщения","phone"}, {"driver_combat_hud","Бой в транспорте","vehicle_combat"},
    {"car hud","Приборная панель","vehicle"}, {"dpad_hint_consumables","Расходники","consumables"},
    {"songbird_phone","Аудиозвонок Сойки","songbird_audio"},
    {"songbird_holocall","Голозвонок","holocall"}, {"hud_courier_bar","Прогресс доставки","courier"},
    {"radio_custom","Радио","radio"},
    {"subtitles","Субтитры","subtitles"},
    {"interactions_root","Диалоги / взаимодействия","dialogs"},
    {"Notifications_Journal","Журнал / уведомления о местности","journal_notifications"},
    {"hud tank","Интерфейс «Василиска»","basilisk"},
    {"q304_dossier","Досье личности","identity_dossier"},
    {"q304_imprint_active_indicator","Отпечаток личности","identity_imprint"},
    {"briefing_sequence_player","Сюжетные брифинги / отпечаток","briefing_sequence"},
    {"scanner_details","Данные сканера","scanner_data"},
    {"camera_hud","Интерфейс камеры наблюдения","surveillance"},
    {"scanner","Прицел / прогресс сканера","scanner_reticle"}
};
inline constexpr size_t ElementCount=std::size(Elements);
constexpr uint64_t NameHash(std::string_view name) {
    uint64_t hash=14695981039346656037ull;
    for (unsigned char c:name) { hash^=c; hash*=1099511628211ull; }
    return hash;
}
inline int ElementIndex(uint64_t name) {
    for(size_t i=0;i<ElementCount;++i) if(NameHash(Elements[i].name)==name) return static_cast<int>(i);
    return -1;
}
struct ElementSettings {
    float x=0, y=0; // percent of canvas width / height
    float scale=1, opacity=1;
    int visible=1;
};
using LayoutSettings=std::array<ElementSettings,ElementCount>;
struct ElementStatus { bool available=false; float x=0,y=0,width=0,height=0; };
using LayoutStatus=std::array<ElementStatus,ElementCount>;
struct Rect { float x=0,y=0,width=0,height=0; };
struct LayoutLatch {
    Rect last{},candidate{};
    bool valid=false, haveCandidate=false;
    static bool Same(Rect a,Rect b) {
        return std::abs(a.x-b.x)<0.05f && std::abs(a.y-b.y)<0.05f &&
               std::abs(a.width-b.width)<0.05f && std::abs(a.height-b.height)<0.05f;
    }
    Rect Update(Rect value,bool dirty) {
        // UI layout jobs may be between updating the slot and its parents.
        // Keep a committed rectangle until layout settles or repeats coherently.
        if(!valid || !dirty || (haveCandidate && Same(candidate,value))) {last=value;valid=true;}
        candidate=value;haveCandidate=true;return last;
    }
};
inline ElementSettings Sanitize(ElementSettings s) {
    s.x=std::isfinite(s.x)?std::clamp(s.x,-100.0f,100.0f):0;
    s.y=std::isfinite(s.y)?std::clamp(s.y,-100.0f,100.0f):0;
    s.scale=std::isfinite(s.scale)?std::clamp(s.scale,0.1f,3.0f):1;
    s.opacity=std::isfinite(s.opacity)?std::clamp(s.opacity,0.0f,1.0f):1;
    s.visible=s.visible!=0; return s;
}
inline Rect ApplyElement(Rect r, ElementSettings s,float canvasW,float canvasH) {
    s=Sanitize(s);
    // Scaling around the element centre keeps a size edit from shifting its anchor.
    const float w=r.width*s.scale,h=r.height*s.scale;
    return {r.x+(r.width-w)*0.5f+s.x*canvasW*0.01f,
            r.y+(r.height-h)*0.5f+s.y*canvasH*0.01f,w,h};
}
LayoutSettings GetLayoutSettings();
void SetLayoutSettings(const LayoutSettings& settings);
LayoutStatus GetLayoutStatus();
void SetLayoutStatus(const LayoutStatus& status);
bool ParseLayoutSetting(const char* line,LayoutSettings& settings);
void SaveLayoutSettings(FILE* file,const LayoutSettings& settings);
}

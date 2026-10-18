// OverlayPanels -- every control the headset overlay draws, and the small widgets they are made of.
//
// The widgets exist because ImGui has no clamped-int slider that reports change the way this file needs:
// CheckboxInt, SliderIntClamped and InputIntClamped all return whether the value moved, so a panel can
// persist only on change instead of writing the ini every frame.
//
// THE PANELS EDIT A SNAPSHOT, NOT THE LIVE CONTROLS. DrawLiveControls takes a LiveControlsUiState by
// reference, and the caller hands it back to be persisted. The live values are volatile scalars read by
// hooks on other threads; editing them directly from the UI thread would mean a slider drag is a series
// of half-applied states, which is visible in the headset.

#include "Anim/WheelGrab.hpp"
#include "Overlay/PpdPanel.hpp"
#include "Framegen/SrNativeApi.hpp"
#include "Framegen/NativeFgObservation.hpp"
#include "Runtimes/HybridBodyYaw.hpp"
#include "Anim/VehiclePosePolicy.hpp"
#include "Utils/SharedSlots.hpp"
#include "Core/VrCoreShared.hpp"
#include "Overlay/ImGuiOverlay.hpp"
#include "Overlay/LiveControlsUi.hpp"
#include "Runtimes/OpenXRManager.hpp"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>
#include "im3d.h"
#include "Overlay/OverlayInternal.hpp"
#include "Overlay/VrWidgets.hpp"
#include "Utils/DebugGate.hpp"
#include "Camera/CameraState.hpp"
#include "Hooks/Reflex.hpp"

extern volatile int g_verboseLog; // per-frame log spam toggle (default off)
extern void Log(const char* fmt, ...);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
extern volatile float g_lastLocateQuat[4];
extern "C" int   CyberpunkVR_StereoModuleEnable;   // vr_core.cpp: did we install at all
extern "C" int   CyberpunkVR_StereoModuleLoaded;
extern "C" int32_t CyberpunkVR_StereoLog;
extern "C" int      CyberpunkVR_StereoSubmit;              // openxr_frameloop.cpp
extern "C" int32_t  CyberpunkVR_StereoEyeCapture;
extern "C" uint32_t CyberpunkVR_StereoEyeMaxAgeMs;
extern "C" uint32_t CyberpunkVR_DebugVrcamEyeAgeMs;        // 0xFFFFFFFF = never produced
extern "C" unsigned long long CyberpunkVR_DebugStereoEyeSubmits;
extern "C" int32_t CyberpunkVR_StableCopy;
extern "C" int32_t CyberpunkVR_StableFromTonemap;
extern "C" uint64_t CyberpunkVR_DebugStableCopies;
extern "C" uint64_t CyberpunkVR_DebugStableSkips;
extern "C" int32_t CyberpunkVR_VrcamDlss;
extern "C" int32_t CyberpunkVR_ForceVrcamCam;
extern "C" uint32_t CyberpunkVR_VrcamEnabled;
extern "C" void        CyberpunkVR_SetVrcamEnabled(uint32_t on);
extern "C" const char* CyberpunkVR_VrcamComponentName();
extern "C" const char* CyberpunkVR_VrcamCameraName();
extern "C" uint32_t CyberpunkVR_MirrorOutput;
extern "C" uint64_t CyberpunkVR_DebugVrcamNodeHits;   // 0 => the second view never dispatched
extern "C" uint64_t CyberpunkVR_DebugMirrorRtvHits;
extern "C" int CyberpunkVR_IsVrcamViewActive();
extern "C" float CyberpunkVR_DebugMainProjYY;
extern "C" float CyberpunkVR_DebugMainCamFov;
extern "C" float CyberpunkVR_MainAdsZoomFactor;
extern "C" float CyberpunkVR_DebugVrcamWantFov;
extern "C" float CyberpunkVR_DebugVrcamBaseFov;
extern "C" int32_t  CyberpunkVR_ProfEnable;
extern "C" double   CyberpunkVR_ProfFrameMs;
extern "C" double   CyberpunkVR_ProfDispMainMs;
extern "C" double   CyberpunkVR_ProfDispVrcamMs;
extern "C" uint32_t CyberpunkVR_ProfDispMainNodes;
extern "C" uint32_t CyberpunkVR_ProfDispVrcamNodes;
extern "C" void     CyberpunkVR_ProfDumpNodes();
extern "C" int      CyberpunkVR_ProfSnapshotNodes(uint32_t* rva, double* msv, double* msm,
                                                  uint32_t* cv, uint32_t* cm, int maxn);
extern "C" const char* CyberpunkVR_ProfNodeName(uint32_t rva);
extern "C" uint64_t CyberpunkVR_DebugViewKeyMainNodes;
extern "C" uint64_t CyberpunkVR_DebugViewKeyOtherNodes;
extern volatile int32_t g_lastLocatePosFP[3];
extern "C" float CyberpunkVRPort_HalfIpd();
extern "C" float GetGameRenderFovDeg();
extern "C" int CyberpunkVR_MainIsRightEye;
extern "C" UINT GetForcedDisplayModeWidth();
extern "C" UINT GetForcedDisplayModeHeight();

namespace overlay {

bool CheckboxInt(const char* label, int* value) {
    bool checked = *value != 0;
    const bool changed = ImGui::Checkbox(label, &checked);
    if (changed) {
        *value = checked ? 1 : 0;
    }
    return changed;
}

bool SliderIntClamped(const char* label, int* value, int minValue, int maxValue) {
    int temp = *value;
    const bool changed = widgets::SliderInt(label, &temp, minValue, maxValue);
    if (changed) {
        *value = std::clamp(temp, minValue, maxValue);
    }
    return changed;
}

bool InputIntClamped(const char* label, int* value, int minValue, int maxValue) {
    int temp = *value;
    const bool changed = ImGui::InputInt(label, &temp, 1, 64);
    if (changed) {
        *value = std::clamp(temp, minValue, maxValue);
    }
    return changed;
}

bool DrawFovControl(LiveControlsUiState& state) {
    bool changed = false;
    bool useRuntime = state.xrForceFov <= 0.0f;
    if (ImGui::Checkbox("Угол обзора (FOV) из среды OpenXR", &useRuntime)) {
        state.xrForceFov = useRuntime ? 0.0f : 112.0f;
        changed = true;
    }

    if (useRuntime) {
        ImGui::BeginDisabled();
    }
    float fov = state.xrForceFov <= 0.0f ? 112.0f : state.xrForceFov;
    if (widgets::SliderFloat("Угол обзора проекции OpenXR", &fov, 80.0f, 140.0f, "%.1f°")) {
        state.xrForceFov = fov;
        changed = true;
    }
    if (useRuntime) {
        ImGui::EndDisabled();
    }
    ImGui::TextUnformatted("Меняет угол обзора проекционного слоя OpenXR, а не камеры CP2077.");
    return changed;
}

// In-headset VR floating-hands controls: tracking on/off, IK calibration, wrist
// alignment, and the diagnostic dump -- everything that used to live only in the
// desktop CET window. Values are published to the RED4ext arm-IK plugin through
// shared memory (OpenXRManager::SetVRHandCalib). Defaults mirror the plugin's
// baked calibration so the rig behaves identically before anything is touched.
void DrawVRHandsControls() {
    // Tracking toggle (writes shared-mem slot [32]; plugin installs hooks + arms
    // and sets g_VRBind = this value). Must be 4 = full-arm IK (the mode the CET
    // "Start VR Tracking" button uses). Mode 2 is the legacy direct bone-write
    // fallback -> stretched forearm / wrong placement, which is what this was.
    bool vrHandTracking = OpenXRManager::Get().GetVRHandTrackingMode() != 0;
    if (ImGui::Checkbox("Включить отслеживание рук в VR", &vrHandTracking)) {
        OpenXRManager::Get().SetVRHandTrackingMode(vrHandTracking ? 4 : 0);
        PersistLiveControlsNow();
    }
    ImGui::SameLine();
    if (ImGui::Button("Записать диагностику VR")) {
        OpenXRManager::Get().RequestVRDiag();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Сохраняет позы маркеров и костей в vrik_diag.txt (рядом с dxgi.dll)\nдля настройки обратной кинематики (IK) рук. То же, что кнопка\n«Log VR Diag» в CET.");
    }

    ImGui::Separator();
    // Select one body-yaw policy; persist the compatible INI flags together.
    {
        LiveControlsUiState st{};
        GetLiveControlsUiState(&st);
        bool roomscale = st.xrRoomscaleMovement != 0;
        if (ImGui::Checkbox("Физическое перемещение по комнате", &roomscale)) {
            st.xrRoomscaleMovement = roomscale ? 1 : 0;
            SetLiveControlsUiState(&st, 1);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Перемещает капсулу персонажа при реальном горизонтальном движении головы.\nИспользует штатные столкновения и подъём по ступеням; движение стиком или WASD сохраняется.\nПриостанавливается в меню, транспорте и при движении по сценарию.");
        }
        using BodyMode=cvr::body::RotationMode;
        int mode=static_cast<int>(cvr::body::RotationModeFromFlags(st.xrPhysicalBodyRotation,
            st.xrTrackedBodyRotation,st.xrHybridBodyRotation));
        static const char* bodyModes[]{"Только стик / пошаговый поворот","Голова + угол свободного обзора",
            "Шлем + контроллеры","Гибридный (по умолчанию)"};
        ImGui::SetNextItemWidth(410.0f);
        if(widgets::Combo("Поворот тела",&mode,bodyModes,4)){
            st.xrPhysicalBodyRotation=mode==static_cast<int>(BodyMode::HeadCone);
            st.xrTrackedBodyRotation=mode==static_cast<int>(BodyMode::Tracked);
            st.xrHybridBodyRotation=mode==static_cast<int>(BodyMode::Hybrid);
            SetLiveControlsUiState(&st,1);
        }
        if(ImGui::IsItemHovered())ImGui::SetTooltip("Гибридный режим: при прямой позе действует угол свободного обзора головы.\nВзгляд вниз на 10 градусов или наклон тела на 5 градусов включает режим «Шлем + контроллеры».\nВозврат при углах менее 8 / 3 градусов; при переключении сохраняется текущее направление тела.\nВ режиме «Шлем + контроллеры» зоны свободного обзора тела всегда отключены. При потере отслеживания направление сохраняется.");
        const bool trackedBody=mode==static_cast<int>(BodyMode::Tracked);
        const bool hybridBody=mode==static_cast<int>(BodyMode::Hybrid);
        const bool bodyRot=mode!=static_cast<int>(BodyMode::Off);
        // Cutscene VRIK suspend (PR #40). Picks the minimum scene tier at which the plugin fully
        // suspends the body+arm solve so the engine authored cinematic pose plays clean. Persisted
        // through the LiveControls bridge (vrport.ini xr_cutscene_suspend_tier), read directly by
        // AnimPose, so a change here takes effect without a restart.
        //
        // Combo index -> stored min-tier: Never(-1), Tier2+(1), Tier3+(2), Tier4+(3), Tier5(4).
        static const int kTierValues[] = { -1, 1, 2, 3, 4 };
        static const char* kTierLabels[] = {
            "Никогда (VRIK всегда включён)",
            "Постановочные сцены и выше (уровень 2+)",
            "Уровень 3 и выше",
            "Кинематографические сцены (уровень 4+) [по умолчанию]",
            "Только полностью постановочные сцены (уровень 5)",
        };
        auto sceneSuspendControl=[&](const char* label,const char* id,int& tier,const char* help) {
            int idx=tier==0 ? 0 : 3;
            for(int i=0;i<5;++i)if(kTierValues[i]==tier) { idx=i;break; }
            const std::string controlLabel=std::string(label)+id;
            if(widgets::Combo(controlLabel.c_str(),&idx,kTierLabels,5)) {
                tier=kTierValues[idx];SetLiveControlsUiState(&st,1);
            }
            if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",help);
        };
        sceneSuspendControl("Отключать VRIK в сценах (пешком)","##cutsceneSuspend",
            st.xrCutsceneSuspendTier,
            "Действует только при передвижении пешком. Начиная с этого уровня VRIK уступает управление анимации сцены.\nПорог для транспорта настраивается отдельно ниже.");
        sceneSuspendControl("Отключать VRIK в сценах (транспорт)","##cutsceneSuspendVehicle",
            st.xrVehicleCutsceneSuspendTier,
            "Действует в транспорте, включая стрельбу пассажира из окна.\nНачиная с этого уровня VRIK уступает управление анимации сцены. При выборе «Никогда» VRIK остаётся включённым.");
        ImGui::BeginDisabled(!bodyRot || trackedBody);
        float cone = st.xrBodyFreeLookDeg;
        if (widgets::SliderFloat("Угол свободного обзора", &cone, 0.0f, 60.0f, "%.0f°")) {
            st.xrBodyFreeLookDeg=cone;SetLiveControlsUiState(&st,1);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("На сколько можно повернуть голову, прежде чем тело начнёт разворачиваться вслед.\nТело плавно поворачивается к центру, затем восстанавливает полную зону свободного обзора.\nПо умолчанию: 10 градусов. Гибридный режим использует это значение при взгляде вперёд.");
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(mode!=static_cast<int>(BodyMode::HeadCone));
        float downCone=st.xrBodyFreeLookDownDeg;
        if(widgets::SliderFloat("Угол свободного обзора при взгляде вниз",&downCone,0.0f,90.0f,"%.0f°")) {
            st.xrBodyFreeLookDownDeg=downCone;SetLiveControlsUiState(&st,1);
        }
        if(ImGui::IsItemHovered())ImGui::SetTooltip("Даёт больше свободы, чтобы дотянуться до предметов на груди.\nПлавно расширяет зону при наклоне головы вниз от 10 до 30 градусов.\nНе сужает обычную зону; задайте такое же значение, чтобы отключить расширение.");
        float swimCone=st.xrBodyFreeLookSwimDeg;
        if(widgets::SliderFloat("Угол свободного обзора в воде",&swimCone,0.0f,90.0f,"%.0f°")) {
            st.xrBodyFreeLookSwimDeg=swimCone;SetLiveControlsUiState(&st,1);
        }
        if(ImGui::IsItemHovered())ImGui::SetTooltip("Один фиксированный угол на поверхности и под водой.\nВзгляд вниз и физический наклон тела не расширяют его. По умолчанию: 5 градусов.");
        ImGui::EndDisabled();
        if(trackedBody)ImGui::TextDisabled("В режиме «Шлем + контроллеры» зоны свободного обзора тела отключены.");
        if(hybridBody)ImGui::TextDisabled("Гибридный режим: угол свободного обзора при прямой позе; шлем + контроллеры при взгляде вниз или наклоне.");
        float radiusCm=st.xrBodyMoveRadius*100;
        if(widgets::SliderFloat("Свободная зона движения тела",&radiusCm,0.0f,15.0f,"%.1f см")) {
            st.xrBodyMoveRadius=radiusCm*.01f;SetLiveControlsUiState(&st,1);
        }
        if(ImGui::IsItemHovered())ImGui::SetTooltip("При небольших реальных перемещениях головы тело остаётся на месте.\nЗа пределами этой зоны тело плавно следует за головой; камера отслеживает её без задержки.");
        if (bodyRot && !trackedBody && cvr::RuntimeDiagnosticsEnabled()) {
            ImGui::Text("активный угол %.1f° | доводка %+.1f° | голова относительно тела %+.1f°",
                        CyberpunkVR_BodyYawFollowDeadDeg,CyberpunkVR_DebugBodyFollowOffsetDeg,CyberpunkVR_DebugBodyFollowErrDeg);
        }
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Калибровка IK рук (R - правая, L - левая)");

    // Defaults mirror the plugin's baked calibration (main.cpp globals).
    static float scaleR = 1.05f, scaleL = 1.06f;   // reach scale (arm straightening)
    static float heightR = 0.0f, heightL = 0.0f; // vertical fine-tune offset (m)
    static float swingR = 1.0f,  swingL = 1.0f;    // elbow-swing gain
    static float poleR = 0.0f,   poleL = 0.0f;     // elbow pole spin (deg)
    static float wRp = 0.0f, wRy = -90.0f, wRr = 0.0f;     // right wrist euler (deg)
    static float wLp = -180.0f, wLy = -90.0f, wLr = 0.0f;  // left wrist euler (deg)

    // TWO-WAY SYNC (fixes "калибровка не сохраняется"). These statics used to be
    // one-way UI -> manager: they kept their hardcoded defaults after the manager
    // loaded vrik_calibration.ini / ran auto-calibration, so the FIRST slider touch
    // (or Apply) pushed 14 default values over the real calibration -- which the next
    // Save then wrote to disk. Pull the manager's live values into the sliders
    // whenever the user isn't actively dragging.
    if (!ImGui::IsAnyItemActive()) {
        float c[14]; OpenXRManager::Get().GetVRHandCalib(c);
        scaleR=c[0]; scaleL=c[1]; heightR=c[2]; heightL=c[3];
        swingR=c[4]; swingL=c[5]; poleR=c[6]; poleL=c[7];
        wRp=c[8]; wRy=c[9]; wRr=c[10]; wLp=c[11]; wLy=c[12]; wLr=c[13];
    }

    bool calChanged = false;
    calChanged |= widgets::SliderFloat("Масштаб длины правой руки", &scaleR, 0.80f, 1.30f, "%.3f");
    calChanged |= widgets::SliderFloat("Масштаб длины левой руки", &scaleL, 0.80f, 1.30f, "%.3f");
    calChanged |= widgets::SliderFloat("Высота правой руки", &heightR, -0.20f, 0.50f, "%.3f м");
    calChanged |= widgets::SliderFloat("Высота левой руки", &heightL, -0.20f, 0.50f, "%.3f м");
    calChanged |= widgets::SliderFloat("Размах правого локтя", &swingR, -3.0f, 3.0f, "%.2f");
    calChanged |= widgets::SliderFloat("Размах левого локтя", &swingL, -3.0f, 3.0f, "%.2f");
    calChanged |= widgets::SliderFloat("Поворот правого локтя", &poleR, -180.0f, 180.0f, "%.1f°");
    calChanged |= widgets::SliderFloat("Поворот левого локтя", &poleL, -180.0f, 180.0f, "%.1f°");

    ImGui::Separator();
    ImGui::TextUnformatted("Смещение поворота кисти (выравнивание ладони/пальцев, градусы)");
    calChanged |= widgets::SliderFloat("Правая кисть: наклон вверх/вниз", &wRp, -180.0f, 180.0f, "%.1f");
    calChanged |= widgets::SliderFloat("Правая кисть: поворот влево/вправо",   &wRy, -180.0f, 180.0f, "%.1f");
    calChanged |= widgets::SliderFloat("Правая кисть: крен",  &wRr, -180.0f, 180.0f, "%.1f");
    calChanged |= widgets::SliderFloat("Левая кисть: наклон вверх/вниз", &wLp, -180.0f, 180.0f, "%.1f");
    calChanged |= widgets::SliderFloat("Левая кисть: поворот влево/вправо",   &wLy, -180.0f, 180.0f, "%.1f");
    calChanged |= widgets::SliderFloat("Левая кисть: крен",  &wLr, -180.0f, 180.0f, "%.1f");

    ImGui::Separator();
    // Auto-calibration: T-pose sample from the same controller poses that draw the gizmo hands.
    // Press "Start", stretch arms out to the sides, stand straight. We derive shoulder offsets
    // and arm scale, then save to vrik_calibration.ini next to dxgi.dll.
    int  cState = OpenXRManager::Get().GetCalibrationState();
    if (cState == 1) {
        float prog = OpenXRManager::Get().GetCalibrationProgress();
        ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.3f, 1.0f),
                           "АВТОКАЛИБРОВКА: вытяните руки СТРОГО В СТОРОНЫ,");
        ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.3f, 1.0f),
                           "встаньте прямо, лицом вперёд. Держите позу %.1f с (%.0f%%).",
                           (1.0f - prog) * 4.0f, prog * 100.0f);
        ImGui::ProgressBar(prog, ImVec2(280, 0));
    } else {
        if (ImGui::Button("Автокалибровка (Т-поза, 4 с)", ImVec2(280, 0))) {
            OpenXRManager::Get().StartAutoCalibration(4.0f);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Нажмите один раз, затем разведите ОБЕ отслеживаемые руки в стороны на уровне плеч\nи стойте прямо лицом вперёд. Мод измерит видимые положения контроллеров,\nвычислит центры вращения плеч и масштаб длины рук и сохранит результат\nв vrik_calibration.ini.");
        }
        if (cState == 2) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Сохранено.");
        }
    }

    ImGui::Separator();
    // Neutral rig alignment is automatic; this button only clears optional trims.
    {
        float cb[3]; OpenXRManager::Get().GetCameraOffset(cb);
        ImGui::TextUnformatted("Камера: на 15 см вперёд от шеи и на 10 см выше глаз");
        if (ImGui::Button("Вернуть камеру к шее", ImVec2(180, 0))) {
            OpenXRManager::Get().BakeCameraOffset();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Использует нейтральное положение шеи: 15 см вперёд и 10 см выше опорного положения глаз.\nСбрасывает поправки камеры; специальная поза, направление головы и замер положения не нужны.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Сбросить поправки##cambake", ImVec2(90, 0))) {
            OpenXRManager::Get().ClearCameraOffset();
            OpenXRManager::Get().SaveCalibrationToFile();
        }
        ImGui::Text("крепление камеры: вправо %.3f  вперёд %.3f  вверх %.3f", cb[0], cb[1], cb[2]);
    }

    ImGui::Separator();
    bool apply  = ImGui::Button("Применить калибровку");
    ImGui::SameLine();
    bool load   = ImGui::Button("Загрузить");
    ImGui::SameLine();
    if (ImGui::Button("Сбросить настройки")) {
        scaleR = 1.05f; scaleL = 1.06f; heightR = 0.0f; heightL = 0.0f;
        swingR = 1.0f; swingL = 1.0f; poleR = 0.0f; poleL = 0.0f;
        wRp = 0.0f; wRy = -90.0f; wRr = 0.0f; wLp = -180.0f; wLy = -90.0f; wLr = 0.0f;
        OpenXRManager::Get().SetShoulderAnatomical(0.14f, -0.17f, 0.05f, -0.14f, -0.17f, 0.05f);
        calChanged = true;
    }

    if (calChanged || apply) {
        OpenXRManager::Get().SetVRHandCalib(scaleR, scaleL, heightR, heightL,
                                            swingR, swingL, poleR, poleL,
                                            wRp, wRy, wRr, wLp, wLy, wLr);
    }
    // AUTOSAVE: persist edits once the drag/press is released, so tweaks survive a
    // restart without requiring the user to remember the Save button.
    {
        static bool s_calDirty = false;
        if (calChanged) s_calDirty = true;
        if (s_calDirty && !ImGui::IsAnyItemActive()) {
            OpenXRManager::Get().SaveCalibrationToFile();
            s_calDirty = false;
        }
    }
    if (load) {
        // The two-way sync above pulls the loaded values into the sliders next frame.
        OpenXRManager::Get().LoadCalibrationFromFile();
    }
}

void ReleaseGameMouseCapture() {
    ClipCursor(nullptr);
    ReleaseCapture();

    CURSORINFO cursorInfo{sizeof(CURSORINFO)};
    if (GetCursorInfo(&cursorInfo) && (cursorInfo.flags & CURSOR_SHOWING) == 0) {
        while (ShowCursor(TRUE) < 0) {
        }
    }
}

void UpdateImGuiMouseFromCursor(HWND hwnd, float backbufferWidth, float backbufferHeight) {
    ImGuiIO& io = ImGui::GetIO();

    RECT client{};
    POINT cursor{};
    if (hwnd && GetClientRect(hwnd, &client) && GetCursorPos(&cursor) && ScreenToClient(hwnd, &cursor)) {
        io.AddMousePosEvent(
            static_cast<float>(cursor.x),
            static_cast<float>(cursor.y));
    }

    io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
    io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
    io.AddMouseButtonEvent(2, (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0);
}

// Stereo panel, carried over from the testbed overlay (testbed/src/overlay_imgui.cpp).
//
// Nothing here goes through LiveControlsUiState: the engine hooks read these globals on every
// frame, so edits apply immediately and the Save button has nothing to do with them. That is
// the same contract the testbed panel had, and it is what makes this usable for tuning IPD
// with the headset on.
void DrawStereoControls() {
    if (!CyberpunkVR_StereoModuleLoaded) {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "Стереомодуль не установлен.");
        ImGui::TextWrapped("Перехватчики движка не загрузились, поэтому настройки ниже не действуют, а шлем показывает обычное моноизображение. Либо CyberpunkVR_StereoModuleEnable равен 0, либо существует файл bin\\x64\\vrport_nostereo.txt. Подробности - в строке «Stereo:» файла cyberpunkvrport.log.");
        return;
    }

    // Three separate questions, reported separately, because each fails on its own:
    // is the second view rendering, did we get a colour frame from it, and did it reach
    // the headset.
    const uint32_t eyeAge = CyberpunkVR_DebugVrcamEyeAgeMs;
    const bool eyeEverProduced = eyeAge != 0xFFFFFFFFu;
    const bool eyeFresh = eyeEverProduced && eyeAge <= CyberpunkVR_StereoEyeMaxAgeMs;
    const bool submitOn = CyberpunkVR_StereoSubmit != 0;

    // Which eye VRCAM lands in is CyberpunkVR_MainIsRightEye's to decide -- the submit picks
    // eye (MainIsRightEye ? 0 : 1) for it -- so the wording is derived rather than written down.
    // It used to say "right" in four places while the default sent VRCAM to the LEFT eye, which
    // is the sort of label that costs an hour of looking in the wrong place.
    const char* kVrcamEye = CyberpunkVR_MainIsRightEye ? "ЛЕВЫЙ" : "ПРАВЫЙ";
    const char* kMainEye  = CyberpunkVR_MainIsRightEye ? "ПРАВЫЙ" : "ЛЕВЫЙ";

    if (submitOn && eyeFresh) {
        ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.5f, 1.0f),
                           "Стерео активно (%s глаз = VRCAM, %s глаз = MAIN)", kVrcamEye, kMainEye);
    } else if (submitOn) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f),
                           "Сейчас нет второго глаза -> оба глаза получают MAIN (моно)");
        ImGui::TextDisabled("Нормально в меню и при загрузке. Если остаётся в игре: компонент VRCAM выключен, либо в vrcam.json указана камера, которой нет у игрока.");
    } else {
        ImGui::TextDisabled("Отправка стерео выключена -> оба глаза получают MAIN (моно)");
    }
    if (cvr::RuntimeDiagnosticsEnabled() && eyeEverProduced)
        ImGui::TextDisabled("возраст кадра VRCAM %u мс  (устарел после %u)   отправок в глаз %llu",
                            eyeAge, CyberpunkVR_StereoEyeMaxAgeMs,
                            static_cast<unsigned long long>(CyberpunkVR_DebugStereoEyeSubmits));
    else if(cvr::RuntimeDiagnosticsEnabled())
        ImGui::TextDisabled("Глаз VRCAM: пока не создано ни одного кадра");
    ImGui::Separator();

    bool submit = submitOn;
    char submitLabel[64];
    std::snprintf(submitLabel, sizeof(submitLabel), "Отправлять VRCAM в %s глаз", kVrcamEye);
    if (ImGui::Checkbox(submitLabel, &submit)) {
        CyberpunkVR_StereoSubmit = submit ? 1 : 0;
        PersistLiveControlsNow();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Выкл.: прежний режим, MAIN копируется в оба глаза.\nВкл.: %s глаз получает итоговое цветное изображение VRCAM -\nто же, что видно в окне зеркала на мониторе, - с преобразованием\nв формат sRGB цепочки вывода и масштабированием до размера изображения глаза.", kVrcamEye);

    // No separate capture switch: the submit path drives CyberpunkVR_StereoEyeCapture from the
    // checkbox above, so the per-frame snapshot cost appears and disappears with the feature
    // instead of being a second thing to remember to turn off.
    if(cvr::RuntimeDiagnosticsEnabled())ImGui::TextDisabled("копий снимка %llu   пропусков %llu   обращений к VRCAM RTV %llu",
                        static_cast<unsigned long long>(CyberpunkVR_DebugStableCopies),
                        static_cast<unsigned long long>(CyberpunkVR_DebugStableSkips),
                        static_cast<unsigned long long>(CyberpunkVR_DebugMirrorRtvHits));

    int maxAge = static_cast<int>(CyberpunkVR_StereoEyeMaxAgeMs);
    if (widgets::SliderInt("Срок годности кадра глаза (мс)", &maxAge, 33, 1000)) {
        CyberpunkVR_StereoEyeMaxAgeMs = static_cast<uint32_t>(maxAge);
        PersistLiveControlsNow();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Как долго можно использовать последний кадр VRCAM. По истечении срока %s\nглаз переключается на MAIN: замершая картинка в одном глазу при движении\nв другом воспринимается намного хуже, чем моноизображение.", kVrcamEye);

    ImGui::Separator();
    // Read-only on purpose. VRCAM's upscaler mirrors MAIN's, decided at graph build from MAIN's
    // own flags -- a switch here could only ever disagree with the engine.
    ImGui::TextDisabled("VRCAM DLSS: %s (следует за масштабированием MAIN, отдельного переключателя нет)",
                        CyberpunkVR_VrcamDlss ? "ВКЛ" : "выкл");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Включается, когда MAIN повышает разрешение через DLSS: VRCAM получает\nсобственную область рендеринга Streamline и рисуется в разрешении ниже целевого.\nВыключается при DLAA / TAA / отсутствии масштабирования. Меняется\nв настройках графики игры, а не здесь.");
    }
    bool forceCam = CyberpunkVR_ForceVrcamCam != 0;
    if (ImGui::Checkbox("Согласовать проекцию VRCAM с MAIN (FOV / приближение / ближняя / дальняя граница)", &forceCam)) {
        CyberpunkVR_ForceVrcamCam = forceCam ? 1 : 0;
        PersistLiveControlsNow();
    }

    ImGui::Separator();
    // Forces the RTT component's isEnabled through the game's RTTI (the CET side re-asserts it,
    // so it survives a reload/respawn). Off means the engine stops rendering the second view
    // entirely, not just our stereo shift -- which is the cheapest way back to plain mono.
    bool vrcamOn = CyberpunkVR_VrcamEnabled != 0;
    if (ImGui::Checkbox("Компонент VRCAM  (RTTI-переключатель: принудительно ВКЛ/ВЫКЛ)", &vrcamOn)) {
        CyberpunkVR_SetVrcamEnabled(vrcamOn ? 1u : 0u);
        PersistLiveControlsNow();
    }
    ImGui::TextDisabled("компонент %s   камера %s",
                        CyberpunkVR_VrcamComponentName(), CyberpunkVR_VrcamCameraName());
    if(cvr::RuntimeDiagnosticsEnabled())ImGui::TextDisabled("узлов вида: main %llu   прочих %llu   vrcam %llu",
                        static_cast<unsigned long long>(CyberpunkVR_DebugViewKeyMainNodes),
                        static_cast<unsigned long long>(CyberpunkVR_DebugViewKeyOtherNodes),
                        static_cast<unsigned long long>(CyberpunkVR_DebugVrcamNodeHits));

    // Separate second swapchain + window mirroring the VRCAM eye (for OBS / desktop preview).
    // Costs a per-frame copy, so it is off unless asked for.
    bool mirrorOn = CyberpunkVR_MirrorOutput != 0;
    if (ImGui::Checkbox("Зеркало VRCAM  (отдельное окно, для записи)", &mirrorOn)) {
        CyberpunkVR_MirrorOutput = mirrorOn ? 1u : 0u;
        PersistLiveControlsNow();
    }

    // Weapon ADS is not a toggle: the vrcam eye always follows MAIN's vertical FOV, narrowed by
    // the aim zoom. Read-only here because the numbers are the quickest way to tell a wrong FOV
    // from a stale one.
    ImGui::TextDisabled("VRCAM FOV %.2f (ресурс %.2f)   MAIN yy %.5f FOV %.2f   прицеливание (ADS) x%.3f",
                        CyberpunkVR_DebugVrcamWantFov, CyberpunkVR_DebugVrcamBaseFov,
                        CyberpunkVR_DebugMainProjYY, CyberpunkVR_DebugMainCamFov,
                        CyberpunkVR_MainAdsZoomFactor);

    if (ImGui::CollapsingHeader("Диагностика")) {
        if (ImGui::Checkbox("Компактная телеметрия камеры при прицеливании (в шлеме)", &g_showCompactAdsTelemetry)) {
            PersistLiveControlsNow();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Небольшая панель, которая остаётся после закрытия F10 и показывает, как ДВИЖОК изменил камеру при прицеливании:\nсмещение и максимум в сантиметрах, вправо/вперёд/вверх относительно направления взгляда и исходного положения при стрельбе от бедра.\nНа отправку стереоизображения не влияет.");
        }
        if (g_showCompactAdsTelemetry) {
            ImGui::Indent();
            if (widgets::SliderFloat("Телеметрия X", &g_compactAdsTelemetryX, 0.10f, 0.90f, "%.2f")) PersistLiveControlsNow();
            if (widgets::SliderFloat("Телеметрия Y", &g_compactAdsTelemetryY, 0.10f, 0.90f, "%.2f")) PersistLiveControlsNow();
            ImGui::TextDisabled("Относительное положение в изображении глаза (от 0 до 1)");
            ImGui::Unindent();
        }
        ImGui::Separator();

        bool slog = CyberpunkVR_StereoLog != 0;
        if (ImGui::Checkbox("Журнал стерео -> cyberpunkvrport.log", &slog)) {
            CyberpunkVR_StereoLog = slog ? 1 : 0;
            PersistLiveControlsNow();
        }

        bool stable = CyberpunkVR_StableCopy != 0;
        if (ImGui::Checkbox("Снимок итогового VRCAM в отдельной памяти", &stable)) {
            CyberpunkVR_StableCopy = stable ? 1 : 0;
            PersistLiveControlsNow();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Выкл.: напрямую читать временную текстуру движка. Её память выделена\nграфом кадра и повторно используется последующим проходом; из-за этого\nизображение чередовалось между светлым и тёмным. Оставьте включённым.");
        bool fromTonemap = CyberpunkVR_StableFromTonemap != 0;
        if (ImGui::Checkbox("Снимок на узле тонального отображения вместо RenderFinal2D", &fromTonemap)) {
            CyberpunkVR_StableFromTonemap = fromTonemap ? 1 : 0;
            PersistLiveControlsNow();
        }

        bool prof = CyberpunkVR_ProfEnable != 0;
        if (ImGui::Checkbox("Профилировщик CPU по узлам (своё и полное время, мс)", &prof)) {
            CyberpunkVR_ProfEnable = prof ? 1 : 0;
            PersistLiveControlsNow();
        }
        ImGui::TextDisabled("кадр %.2f мс   диспетчеризация: main %.2f (%u)  vrcam %.2f (%u)",
                            CyberpunkVR_ProfFrameMs,
                            CyberpunkVR_ProfDispMainMs, CyberpunkVR_ProfDispMainNodes,
                            CyberpunkVR_ProfDispVrcamMs, CyberpunkVR_ProfDispVrcamNodes);
        if (ImGui::Button("Записать аудит узлов в журнал (сбросить интервал)"))
            CyberpunkVR_ProfDumpNodes();
        if (prof) {
            // Top-15 by SELF time. Self is the only rankable column: inclusive double-counts,
            // because SceneDrv contains every scene pass it dispatches.
            static uint32_t rva[15];
            static double msv[15], msm[15];
            static uint32_t cv[15], cm[15];
            const int n = CyberpunkVR_ProfSnapshotNodes(rva, msv, msm, cv, cm, 15);
            ImGui::TextDisabled("топ %d по собственным мс/кадр   (main | vrcam)", n);
            for (int i = 0; i < n; ++i) {
                const char* nm = CyberpunkVR_ProfNodeName(rva[i]);
                ImGui::Text("%-30s %6.3f | %6.3f", (nm && nm[0]) ? nm : "?", msm[i], msv[i]);
            }
        }
    }
}

static bool DrawDrivingControls(LiveControlsUiState& state) {
    bool changed=false;
            ImGui::Separator();
            ImGui::TextUnformatted("Вождение - руки на руле");
            changed |= CheckboxInt("Брать руль кнопками захвата", &state.xrWheelGrab);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Во время ВОЖДЕНИЯ поднесите руку туда, где анимация удерживает руль\nавтомобиля или мотоцикла, и нажмите кнопку захвата: рука перейдёт\nпод управление штатной анимации игры - ладонь на руле, пальцы обхватывают его -\nвместо следования за контроллером. Отпустите кнопку захвата, и рука снова\nбудет следовать за вашей рукой.\n\nРуки независимы: можно держать руль одной, а оружие - другой.\nПока рука на руле, её кнопка захвата не выполняет других действий\n(не достаёт оружие из кобуры и не берёт магазин).");
            }
            {
                float r = state.xrWheelRadius > 0.0f ? state.xrWheelRadius : 0.28f;
                if (widgets::SliderFloat("Радиус захвата (м)", &r, 0.08f, 0.60f, "%.2f")) {
                    state.xrWheelRadius = r;
                    changed = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Насколько близко ваша рука должна быть к руке в анимации, чтобы нажатие\nкнопки захвата считалось захватом руля. Больший радиус помогает найти\nневидимый руль; слишком большой приводит к захвату руля при любом нажатии в машине.");
                }
                float m = state.xrWheelSteerMaxDeg > 0.0f ? state.xrWheelSteerMaxDeg : 90.0f;
                if (widgets::SliderFloat("Полный выворот при (°)", &m, 30.0f, 120.0f, "%.0f")) {
                    state.xrWheelSteerMaxDeg = m;
                    changed = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Чувствительность руля: поворот руки от точки захвата до полного выворота.\nДля одной руки центр вращения калибруется по точке захвата, для двух используется линия между ними.\nДобавление или отпускание руки сохраняет текущий поворот.\n90 = четверть оборота до полного выворота. Меньше = чувствительнее.\nПоворот за предел полного выворота не смещает нейтральное положение.");
                }
                float d = state.xrWheelSteerDeadDeg >= 0.0f ? state.xrWheelSteerDeadDeg : 1.5f;
                if (widgets::SliderFloat("Мёртвая зона руля (°)", &d, 0.0f, 20.0f, "%.1f")) {
                    state.xrWheelSteerDeadDeg = d;
                    changed = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Отклонение от центра, при котором колёса ещё не поворачиваются.\nУвеличьте, если машину уводит, когда вы держите руль прямо;\nкаждый градус добавляет градус свободного хода вокруг центра.\nПолный диапазон по-прежнему заканчивается на «Полный выворот при»,\nпоэтому расширение мёртвой зоны не вызывает скачка руления.");
                }

                changed |= CheckboxInt("Предсказание поворота руля", &state.xrWheelPrediction);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Мягко предсказывает продолжение поворота по недавнему движению руки.\nОграничено 8 мс и 0,5 градуса; нейтральное положение не меняется.\nПрекращается при остановке, смене направления или изменении захвата.\nПо умолчанию выключено. Начало поворота предсказать не может.");
                }
                if (state.xrWheelPrediction) {
                    changed |= widgets::SliderFloat("Упреждение поворота (мс)", &state.xrWheelPredictionMs, 0.0f, 8.0f, "%.1f");
                }
                changed |= CheckboxInt("Сигнал при касании центра руля", &state.xrWheelHorn);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "Во время ВОЖДЕНИЯ положите руку на СЕРЕДИНУ руля - туда, где находится\nкнопка сигнала в настоящей машине. Сигнал звучит, пока рука остаётся там.\nКнопку захвата нажимать не нужно; рука, УДЕРЖИВАЮЩАЯ руль, не включает сигнал.");
                }
                float hr = state.xrWheelHornRadius > 0.0f ? state.xrWheelHornRadius : 0.12f;
                if (widgets::SliderFloat("Радиус зоны сигнала в центре руля (м)", &hr, 0.04f, 0.30f, "%.2f")) {
                    state.xrWheelHornRadius = hr;
                    changed = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Насколько близко к центру руля должна быть рука для включения сигнала.\nБольший радиус облегчает поиск на ощупь; слишком большая зона\nдостигает обода, и каждый захват руля включает сигнал.");
                }

                ImGui::Spacing();
                changed |= CheckboxInt("Курок стреляет из транспорта", &state.xrVehicleGunTrigger);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "Достаньте оружие на водительском месте: ПРАВЫЙ КУРОК перестанет управлять\nгазом и станет спуском оружия. Держите руль левой рукой и стреляйте правой.\n\nГаз ФИКСИРУЕТСЯ на уровне, который был при извлечении оружия, и машина\nпродолжает ехать. ЛЕВЫЙ СТИК вперёд/назад подстраивает скорость, пока вы\nстреляете. Уберите оружие в кобуру - курок снова управляет газом.\n\nПока оружие в руке, движение левого стика вперёд/назад занято подстройкой:\nнаклоны / раскачивание и жест автопилота недоступны до возвращения оружия в кобуру.");
                }
                float tt = state.xrVehicleThrottleTrim > 0.0f ? state.xrVehicleThrottleTrim : 0.5f;
                if (widgets::SliderFloat("Скорость подстройки газа (/с)", &tt, 0.05f, 3.0f, "%.2f")) {
                    state.xrVehicleThrottleTrim = tt;
                    changed = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Какую долю полного диапазона газа левый стик добавляет или убавляет\nза секунду, пока оружие в руке.\n0,5 = две секунды удержания от нулевого до полного газа.");
                }

                // LIVE STATE, so "it did not grab" and "it steers the wrong way" are both answerable
                // without a log: the armed mask is raised by proximity, the blends by the grab itself,
                // and the angle is what the stick is being driven from.
                if(cvr::RuntimeDiagnosticsEnabled()) {
                    const int mask = static_cast<int>(
                        OpenXRManager::Get().GetSharedSlot(vrshared::kWheelArmedMask));
                    const int horn = cvr::anim::g_wheelHornMask.load(std::memory_order_relaxed);
                    ImGui::Text("за рулём %d   у руля  L %d R %d   удержание  L %.2f R %.2f",
                                g_isDriving.load(std::memory_order_relaxed) ? 1 : 0,
                                (mask & vrshared::kWheelArmedLeftBit) ? 1 : 0,
                                (mask & vrshared::kWheelArmedRightBit) ? 1 : 0,
                                cvr::anim::g_wheelBlendLeft.load(std::memory_order_relaxed),
                                cvr::anim::g_wheelBlendRight.load(std::memory_order_relaxed));
                    ImGui::Text("руление  %+.1f°  ->  стик %+.2f     сигнал  L %d R %d",
                                cvr::anim::g_wheelSteerDeg.load(std::memory_order_relaxed),
                                cvr::anim::g_wheelSteer.load(std::memory_order_relaxed),
                                (horn & vrshared::kWheelArmedLeftBit) ? 1 : 0,
                                (horn & vrshared::kWheelArmedRightBit) ? 1 : 0);
                }
            }

        ImGui::Separator();
        ImGui::TextUnformatted("СМЕЩЕНИЕ КАМЕРЫ В ТРАНСПОРТЕ");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Смещения головы в разделе «ОБЩЕЕ» калибруются стоя. В сидячем положении\nштатная камера транспорта уже расположена правильно, поэтому порт отключает\nв транспорте две автоматические поправки. Из-за этого смещение для положения\nстоя уводит камеру с сиденья вместо исправления её положения.\n\nЭти три значения добавляются к смещениям головы только в транспорте\nи перестают действовать сразу после выхода: положение в машине и на улице\nможно настроить отдельно. Ноль сохраняет нынешнее смещение камеры в машине.");
        }
        const bool passengerCombat=g_isInVehicle && cvr::anim::IsPassengerWindowCombat(
            g_vehicleState.load(std::memory_order_relaxed));
        ImGui::BeginDisabled(passengerCombat);
        changed |= widgets::SliderFloat("Голова в машине X вправо", &state.xrVehHeadOffsetX, -0.50f, 0.50f, "%.3f м");
        changed |= widgets::SliderFloat("Голова в машине Y вперёд", &state.xrVehHeadOffsetY, -0.50f, 0.50f, "%.3f м");
        changed |= widgets::SliderFloat("Голова в машине Z вверх", &state.xrVehHeadOffsetZ, -0.50f, 0.50f, "%.3f м");
        ImGui::EndDisabled();
        {   // Live, so a slider that is doing nothing says so instead of being blamed.
            ImGui::TextDisabled(passengerCombat ? "   (стрельба из окна: «ОБЩЕЕ» > смещения головы)"
                               : g_isInVehicle ? "   (в транспорте: действуют)"
                                              : "   (пешком: игнорируются)");
        }
    return changed;
}

bool DrawFpsOverlayControls(LiveControlsUiState& state) {
    bool changed=false;auto& fg=state.framegen;
            changed|=ImGui::Checkbox("Панель FPS",&fg.overlay);
            changed|=widgets::Combo("Вид панели FPS",&fg.overlayLayout,"Подробный\0Компактный\0Минимальный\0");
            changed|=widgets::Combo("Положение панели FPS",&fg.overlayCorner,"Сверху слева\0Сверху справа\0Снизу слева\0Снизу справа\0Сверху по центру\0Снизу по центру\0Слева по центру\0Справа по центру\0В центре\0");
            changed|=widgets::SliderFloat("Масштаб панели FPS",&fg.overlayScale,.5f,2,"%.2fx");
            changed|=widgets::SliderFloat("Расстояние до панели FPS",&fg.overlayDistance,.5f,5,"%.2f м");
            changed|=widgets::SliderFloat("Смещение панели FPS по горизонтали",&fg.overlayX,-60,60,"%.1f°");
            changed|=widgets::SliderFloat("Смещение панели FPS по вертикали",&fg.overlayY,-45,45,"%.1f°");
            changed|=CheckboxInt("Свободный обзор относительно панели FPS",&fg.overlayFollow);
            ImGui::BeginDisabled(!fg.overlayFollow);
            changed|=widgets::SliderFloat("Угол свободного обзора панели FPS",&fg.overlayCone,5,90,"%.1f°");
            if(ImGui::IsItemHovered())ImGui::SetTooltip("Сохраняет направление панели в пределах этого угла поворота головы; за его пределами панель догоняет взгляд.");
            ImGui::EndDisabled();
            int history=fg.historySeconds==10?0:(fg.historySeconds==60?2:(fg.historySeconds==120?3:1));
            if(widgets::Combo("Период статистики",&history,"10 секунд\0 30 секунд\0 60 секунд\0 120 секунд\0")) {const int seconds[]={10,30,60,120};fg.historySeconds=seconds[history];changed=true;}
            if(fg.overlay) {
            if(ImGui::Button("Сбросить статистику"))cvr::framegen::ResetStatistics();
            const auto stats=cvr::framegen::GetStatistics();
            ImGui::Text("Реальные %.1f FPS   Генерация %.1f FPS   Вывод %.1f FPS",stats.realFps,stats.generatedFps,stats.outputFps);
            ImGui::Text("Среднее %.1f FPS   Мин. %.1f   Макс. %.1f",stats.averageFps,stats.minimumFps,stats.maximumFps);
            if(stats.low1Fps>0)ImGui::Text("Медленные 1%%: %.1f FPS",stats.low1Fps);else ImGui::TextDisabled("Медленные 1%%: собираем 100 интервалов реальных кадров");
            if(stats.low01Fps>0)ImGui::Text("Медленные 0,1%%: %.1f FPS",stats.low01Fps);else ImGui::TextDisabled("Медленные 0,1%%: собираем 1000 интервалов реальных кадров");
            const float graphMax=float(std::max(33.34,stats.budgetMs*2));
            ImGui::PlotLines("История времени CPU (мс)",stats.cpuGraph.data(),int(stats.cpuGraph.size()),0,nullptr,0,graphMax,ImVec2(0,55));
            ImGui::PlotLines("История времени GPU (мс)",stats.gpuGraph.data(),int(stats.gpuGraph.size()),0,nullptr,0,graphMax,ImVec2(0,55));
            ImGui::Text("Кадр CPU %.2f мс   Пик %.2f мс",stats.cpuMs,stats.cpuPeakMs);
            if(stats.gpuValid)ImGui::Text("Кадр GPU %.2f мс   Пик %.2f мс",stats.gpuMs,stats.gpuPeakMs);
            else ImGui::TextDisabled("Время кадра GPU: ожидание завершения измерений");
            ImGui::Text("Генерация %.2f мс   Учтённая VRAM %.1f МиБ",stats.generationMs,double(stats.vramBytes)/(1024*1024));
            const auto& hw=stats.hardware;constexpr double gib=1024.0*1024*1024;
            ImGui::Text("Процессор: %s",hw.cpuName[0]?hw.cpuName:"--");
            ImGui::Text("Видеокарта: %s",hw.gpuName[0]?hw.gpuName:"--");
            if(hw.cpuUsage>=0)ImGui::Text("Загрузка CPU %.1f%%   Самое загруженное логическое ядро %.1f%%",hw.cpuUsage,hw.coreMaximum);
            if(hw.gpuUsage>=0)ImGui::Text("Загрузка GPU %.1f%%   Температура %.0f C",hw.gpuUsage,hw.gpuTemperature);
            if(hw.gpuMemoryValid)ImGui::Text("Видеопамять %.2f / %.2f ГиБ",hw.gpuMemoryUsed/gib,hw.gpuMemoryTotal/gib);
            if(hw.gpuProcessMemoryValid)ImGui::Text("Видеопамять игры %.2f ГиБ   Бюджет %.2f ГиБ",hw.gpuProcessUsed/gib,hw.gpuProcessBudget/gib);
            ImGui::Text("ОЗУ %.2f / %.2f ГиБ   Рабочий набор игры %.2f ГиБ",hw.ramUsed/gib,hw.ramTotal/gib,hw.appWorkingSet/gib);
            ImGui::Text("Глубина + движение: %s   Пропущено %llu",stats.inputsReady?"получены для обоих глаз":"ожидание",stats.skipped);
            ImGui::TextDisabled("CPU: интервалы захваченных кадров. GPU: интервалы кадров в графической очереди. Пики: за последние 2 секунды.");
            ImGui::TextDisabled("FPS вывода не учитывает повторные кадры и не измеряет частоту фактического показа в шлеме.");
            ImGui::TextDisabled("Показатели медленных 1%% / 0,1%% используют среднее самых долгих интервалов реальных кадров. Опрос оборудования: 1 Гц.");
            } else ImGui::TextDisabled("Панель FPS и статистика выключены.");
    return changed;
}

bool DrawLiveControls(LiveControlsUiState& state,int section) {
    bool changed = false;

    if (ImGui::Button("Центрировать шлем (F7)")) {
        RequestLiveControlsRecenter();
    }
    if (section>=0 || ImGui::BeginTabBar("CyberpunkVRPortTabs")) {
        if (section>=0 ? section==0 : ImGui::BeginTabItem("ОБЩЕЕ")) {
            if (ImGui::CollapsingHeader("ИГРОВЫЕ МЕНЮ", ImGuiTreeNodeFlags_DefaultOpen)) {
                changed |= CheckboxInt("Плоскость VR-меню", &state.xrMenuRect);
                changed |= widgets::SliderFloat("Угловой размер VR-меню", &state.xrMenuFov, 30.0f, 120.0f, "%.1f°");
            }

            if (ImGui::CollapsingHeader("Отслеживание / камера")) {
        ImGui::TextUnformatted("Направление движения задаётся на вкладке «Управление».");
        // "Fix Head" removed. It switched the view to 3DoF, and it did not stop at dropping the
        // head translation -- it dropped these three offsets and the calibration bakes with it,
        // then hid the very sliders that were needed to put the view right. The offsets are
        // always live now, and they reach BOTH eyes.
        changed |= widgets::SliderFloat("Голова X вправо", &state.xrHeadOffsetX, -0.50f, 0.50f, "%.3f м");
        changed |= widgets::SliderFloat("Голова Y вперёд", &state.xrHeadOffsetY, -0.50f, 0.50f, "%.3f м");
        changed |= widgets::SliderFloat("Голова Z вверх", &state.xrHeadOffsetZ, -0.50f, 0.50f, "%.3f м");

            }

            if (ImGui::CollapsingHeader("Отладочные гизмо")) {
                ImGui::TextUnformatted("Исходные маркеры рук / отладочные элементы:");
                if (ImGui::Checkbox("Показывать маркеры рук", &g_drawHandLocator)) PersistLiveControlsNow();
                if (ImGui::Checkbox("Показывать упрощённые 3D-модели рук", &g_drawHandProxy3D)) PersistLiveControlsNow();
                if (ImGui::Checkbox("Показывать отладочный каркас и оси", &g_drawHandDebugAxes)) PersistLiveControlsNow();
                if (widgets::SliderFloat("Масштаб локатора", &g_handLocatorScale, 0.50f, 2.00f, "%.2f")) PersistLiveControlsNow();
            }

            if (ImGui::CollapsingHeader("Диагностика")) {
        { int vl = GetLiveVerboseLogSetting(); if (CheckboxInt("Подробный журнал (шумная диагностика)", &vl)) SetLiveVerboseLogSetting(vl); }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("По умолчанию выключено, чтобы не засорять cyberpunkvrport.log. Включайте\nтолько для диагностики ClipCursor, глубины и перехватчиков.\nПока настройка здесь не изменена, используется значение DEBUG из лаунчера.");
        }
            }
            if(section<0)ImGui::EndTabItem();
        }

        if (section>=0 ? section==1 : ImGui::BeginTabItem("ГЕНЕРАЦИЯ КАДРОВ")) {
            auto& fg=state.framegen;
            int stereoMode=fg.aerV2?1:0;
            if(widgets::Combo("Камеры",&stereoMode,"Две камеры\0Одна камера — AER v2\0")) {
                fg.aerV2=stereoMode==1;changed=true;
            }
            if(ImGui::IsItemHovered())ImGui::SetTooltip("AER v2 чередует настоящий рендер левого и правого глаза.\nГенерация промежуточных кадров включается отдельно ниже.");
            cvr::ppd::DrawPpdPanel();
            ImGui::Separator();
            const bool gameFgControls=cvr::framegen::NativeFgStereoRequested();
            if(gameFgControls)ImGui::TextWrapped("NVIDIA FG включается и настраивается в графических настройках игры. Фактические сгенерированные кадры показаны в статистике.");
            if(gameFgControls)ImGui::TextWrapped("Тестовая сборка: после смены режима камер перезапустите игру, чтобы генерация получила отдельные истории глаз.");
            ImGui::BeginDisabled(gameFgControls);
            changed|=ImGui::Checkbox("Включить генерацию кадров",&fg.enabled);
            if(ImGui::IsItemHovered())ImGui::SetTooltip("При включённой генерации кадров сглаживание движений рук автоматически отключается.");
            int backend=int(fg.backend);
            if(widgets::Combo("Генератор кадров",&backend,"FidelityFX\0NVIDIA OFA + FidelityFX\0Наш NVIDIA\0")) {fg.backend=cvr::framegen::Backend(backend);changed=true;}
            if(fg.backend==cvr::framegen::Backend::OwnNvidia) {
                changed|=widgets::SliderInt("Промежуточных кадров",&fg.ownGeneratedFrames,1,5);
                ImGui::TextWrapped("Это запрошенное количество между настоящими кадрами одного глаза. Фактическая генерация и подача проверяются в статистике.");
            } else {
            if(fg.backend==cvr::framegen::Backend::Nvidia)
                changed|=widgets::Combo("Качество оптического потока NVIDIA",&fg.quality,"Быстро (производительность)\0Средне (баланс)\0Медленно (лучшее качество)\0");
            int scale=fg.flowScale==100?2:(fg.flowScale==75?1:0);
            if(widgets::Combo("Разрешение оптического потока",&scale,"50%\0 75%\0 100%\0")) {fg.flowScale=scale==2?100:(scale==1?75:50);changed=true;}
            changed|=ImGui::Checkbox("Реальные кадры с половиной частоты обновления шлема",&fg.autoPace);
            ImGui::TextWrapped("Использует игровые векторы движения и глубину для обоих глаз. Добавляет один промежуточный кадр между реальными.");
            }
            ImGui::EndDisabled();
            ImGui::Separator();
            const auto runtimeStatus=cvr::framegen::GetRuntimeStatus();
            ImGui::TextWrapped("%s",runtimeStatus.text);
            if(section<0)ImGui::EndTabItem();
        }

        if (section>=0 ? section==2 : ImGui::BeginTabItem("ИНТЕРФЕЙС (HUD)")) {
            changed |= CheckboxInt("Включить панель HUD", &state.xrHudPanel);
            const char* followModes[] = {"Голова / угол свободного обзора", "Поворот тела"};
            changed |= widgets::Combo("Следовать за", &state.xrHudFollowMode, followModes, 2);
            ImGui::BeginDisabled(state.xrHudFollowMode != 0);
            changed |= widgets::SliderFloat("Угол свободного обзора", &state.xrHudFollowDeg, 5.0f, 90.0f, "%.1f°");
            if(ImGui::IsItemHovered())ImGui::SetTooltip("Догоняет взгляд за пределами зоны свободного обзора, либо через 3 секунды покоя, если внутри зоны отклонение по горизонтали превышает 10 градусов.");
            ImGui::EndDisabled();
            changed |= widgets::SliderFloat("Угловой размер HUD", &state.xrHudFov, 30.0f, 120.0f, "%.1f°");
            changed |= CheckboxInt("Стереоскопическая глубина HUD", &state.xrHudStereoDepth);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Выкл.: HUD направлен одинаково для обоих глаз.\nВкл.: HUD расположен на выбранном физическом расстоянии.");
            ImGui::BeginDisabled(!state.xrHudStereoDepth);
            changed |= widgets::SliderFloat("Расстояние до HUD", &state.xrHudDistance, 0.5f, 5.0f, "%.2f м");
            ImGui::EndDisabled();
            changed |= widgets::SliderFloat("Яркость", &state.xrHudBrightness, 0.25f, 3.0f, "%.2fx");
            changed |= widgets::SliderFloat("Тень / контур", &state.xrHudShadow, 0.0f, 2.0f, "%.2fx");
            changed |= widgets::SliderFloat("Свечение", &state.xrHudGlow, 0.0f, 2.0f, "%.2fx");
            ImGui::Separator();
            if(ImGui::CollapsingHeader("ВЗАИМОДЕЙСТВИЯ / ДИАЛОГИ",ImGuiTreeNodeFlags_DefaultOpen)){
                changed |= CheckboxInt("Взаимодействия / диалоги в пространстве мира",&state.xrInteractionPanel);
                changed |= widgets::SliderFloat("Угол свободного обзора взаимодействий",&state.xrInteractionFollowDeg,5.0f,90.0f,"%.1f°");
                changed |= widgets::SliderFloat("Угол свободного обзора добычи",&state.xrLootFollowDeg,5.0f,90.0f,"%.1f°");
                if(ImGui::IsItemHovered())ImGui::SetTooltip("Используется, когда видны панель добычи и описание предмета. По умолчанию: 10 градусов.");
                changed |= widgets::SliderFloat("Расстояние до панели взаимодействий",&state.xrInteractionDistance,.5f,5.0f,"%.2f м");
                changed |= widgets::SliderFloat("Угловой размер панели взаимодействий",&state.xrInteractionFov,30.0f,120.0f,"%.1f°");
                ImGui::TextWrapped("Подсказки взаимодействий и варианты диалога используют общую панель. Она следует за взглядом только за пределами зоны свободного обзора, без отложенного доворачивания в покое.");
            }
            ImGui::Separator();
            static int element=10;
            const char* elementLabels[cvr::hud::ElementCount];
            for(size_t i=0;i<cvr::hud::ElementCount;++i)elementLabels[i]=cvr::hud::Elements[i].label;
            widgets::Combo("Элемент",&element,elementLabels,int(cvr::hud::ElementCount));
            const auto status=cvr::hud::GetLayoutStatus();
            if(!status[element].available) ImGui::TextDisabled("Сейчас не виден в игре; настройки всё равно сохраняются.");
            auto& transform=state.hudElements[element];
            changed |= CheckboxInt("Показывать элемент", &transform.visible);
            changed |= widgets::SliderFloat("Смещение по горизонтали", &transform.x, -100.0f,100.0f,"%.1f %%");
            changed |= widgets::SliderFloat("Смещение по вертикали", &transform.y, -100.0f,100.0f,"%.1f %%");
            changed |= widgets::SliderFloat("Масштаб элемента", &transform.scale,0.1f,3.0f,"%.2fx");
            changed |= widgets::SliderFloat("Непрозрачность", &transform.opacity,0.0f,1.0f,"%.2f");
            if(ImGui::Button("Сбросить элемент")) {transform={};changed=true;}
            ImGui::SameLine();
            if(ImGui::Button("Сбросить все элементы")) {state.hudElements={};changed=true;}
            if(section<0)ImGui::EndTabItem();
        }

        if (section>=0 ? section==3 : ImGui::BeginTabItem("УПРАВЛЕНИЕ")) {
            static int page=0;
            static const char* pages[]={"ОБЩЕЕ","ВОЖДЕНИЕ","НАЗНАЧЕНИЯ КНОПОК"};
            widgets::Tabs("controls-pages",page,pages,3);
            if(page==1)changed|=DrawDrivingControls(state);
            else if(page==2)widgets::DrawBindings(state);
            else {
            widgets::Section("СОЧЕТАНИЯ КНОПОК");
            static const char* chordModes[]{
                "L3 / нажатие левого стика",
                "R3 / нажатие правого стика",
                "Касание правой площадки для пальца"
            };
            state.xrChordActivation=std::clamp(state.xrChordActivation,0,2);
            changed|=widgets::Combo("Кнопка активации сочетания",&state.xrChordActivation,chordModes,IM_ARRAYSIZE(chordModes));
            bool extras=state.xrExtraChordActions!=0;
            if(ImGui::Checkbox("Дополнительные сочетания",&extras)){state.xrExtraChordActions=extras?1:0;changed=true;}
            const int effectiveChordMode=(state.xrChordActivation==2 && !state.xrRightThumbrestAvailable)?0:state.xrChordActivation;
            if(state.xrChordActivation==2 && effectiveChordMode==0)
                ImGui::TextWrapped("Правая площадка для пальца недоступна в активном профиле контроллера. Для сочетаний временно используется L3; выбранная настройка сохраняется.");
            ImGui::TextWrapped("Сначала нажмите и удерживайте кнопку-модификатор, затем выполните действие сочетания. Уже удерживаемые элементы управления сохраняют обычное действие до отпускания.");
            ImGui::Separator();

            ImGui::TextWrapped("Перемещение, прицеливание оружием и быстрые физические взаимодействия.");

            // Weapon aim: bullets/projectiles fly down the WEAPON BARREL (controller-pointed) instead
            // of the camera crosshair. Hooks the projectile launch orientation provider and feeds it
            // the game's own muzzle world transform. Writes shared[58]; the RED4ext plugin applies it.
            {
                bool headAim = OpenXRManager::Get().GetWeaponAimEnable() == 0;
                if (ImGui::Checkbox("Независимое прицеливание головой в VR", &headAim)) {
                    OpenXRManager::Get().SetWeaponAimEnable(headAim ? 0 : 1);
                    PersistLiveControlsNow();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("ВКЛ.: прицеливание шлемом независимо от направления движения (3DoF).\nВЫКЛ.: прицеливание VR-контроллерами (6DoF).\nЕсли включено скрытие лазерной точки при прицеливании, выстрелы следуют линии прицела оружия.");
                }
                if (ImGui::Checkbox("Лазерная точка прицела (куда попадёт пуля)", &g_drawBarrelCross)) {
                    PersistLiveControlsNow();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Красная точка, спроецированная из реального направления дула\nчерез игровую камеру - отмечает ровно то место, куда полетит пуля.");
                }
                static const char* laserModes[] = {
                    "Стабильная проекция",
                    "Точка в пространстве мира",
                    "Луч до поверхности"
                };
                int laserDotMode = std::clamp(state.xrLaserDotMode, 0, 2);
                ImGui::SetNextItemWidth(220.0f);
                if (ImGui::Combo("Режим лазерной точки", &laserDotMode, laserModes, IM_ARRAYSIZE(laserModes))) {
                    state.xrLaserDotMode = laserDotMode;
                    changed = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Стабильная проекция: устойчивый указатель направления ствола.\nТочка в пространстве мира: одна точка на конечном расстоянии проецируется в оба глаза.\nЛуч до поверхности: точка на поверхности мира или персонажа, в которую попал луч из дула.");
                }
                ImGui::SetNextItemWidth(220.0f);
                int laserDotRadiusMm = static_cast<int>(std::lround(state.xrLaserDotRadiusMm));
                if (ImGui::SliderInt("Радиус лазерной точки", &laserDotRadiusMm, 1, 50, "%d мм")) {
                    state.xrLaserDotRadiusMm = static_cast<float>(laserDotRadiusMm);
                    changed = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Радиус лазерной точки в миллиметрах игрового мира.\nРазмер в пикселях подстраивается под текущее разрешение рендеринга и проекцию.");
                }
                ImGui::SameLine();
                const bool distanceScaleAvailable = laserDotMode == 2;
                if (!distanceScaleAvailable) ImGui::BeginDisabled();
                bool scaleWithDistance = state.xrLaserDotScaleWithDistance != 0;
                if (ImGui::Checkbox("Менять размер с расстоянием", &scaleWithDistance)) {
                    state.xrLaserDotScaleWithDistance = scaleWithDistance ? 1 : 0;
                    changed = true;
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("Только для луча до поверхности. Размер светового пятна рассчитывается по фактическому расстоянию до поверхности.");
                }
                if (!distanceScaleAvailable) ImGui::EndDisabled();
                ImGui::Indent();
                if (!g_drawBarrelCross) ImGui::BeginDisabled();
                changed |= CheckboxInt("Скрывать лазерную точку при прицеливании", &state.xrHideLaserDotAds);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("Скрывает внешнюю лазерную точку при прицеливании и направляет выстрел вдоль линии прицела оружия.");
                }
                if (!g_drawBarrelCross) ImGui::EndDisabled();
                ImGui::Unindent();
            }
            ImGui::Separator();

            changed |= CheckboxInt("Включить слияние VR -> XInput", &state.xrXInputHook);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("При каждом опросе объединяет состояние VR-контроллеров с геймпадом XInput 0 (операция OR).\nВыкл.: игра видит только физический геймпад, а при его отсутствии не получает ввод.");
            }

            ImGui::Separator();
            ImGui::TextUnformatted("Кобуры оружия (тянуться + правая кнопка захвата)");
            changed |= CheckboxInt("Кобуры с управлением жестами", &state.xrImmersiveHolsters);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "ВКЛ.: оружие выбирается по ВИДИМОЙ кобуре, к которой вы тянетесь:\n      за правым плечом - основное оружие (винтовка / снайперская винтовка),\n      на бедре с катаной - ближний бой, на бедре с пистолетом - пистолет.\nВЫКЛ.: видимые кобуры игнорируются, каждой зоне назначен фиксированный слот:\n      за плечом - EquipmentSlot1, правое бедро - Slot2, левое бедро - Slot3.\nДотянитесь до зоны и нажмите ПРАВУЮ кнопку захвата, чтобы достать или убрать оружие.");
            }


            ImGui::Separator();
            ImGui::TextUnformatted("Направление перемещения");
            bool analogMovement=state.xrMovementSpeedMode==1;
            if(ImGui::Checkbox("Аналоговое перемещение",&analogMovement)) {state.xrMovementSpeedMode=analogMovement?1:0;changed=true;}
            if(ImGui::IsItemHovered())ImGui::SetTooltip("Вкл. (по умолчанию): скорость пешком зависит от отклонения левого стика. Выкл.: используется текущая схема фиксированной скорости.\nМёртвые зоны стиков и порог полного отклонения ниже действуют в обоих режимах и при вождении.");
            int leftDeadzone=int(state.xrLeftStickDeadzone*100+.5f);
            int rightDeadzone=int(state.xrRightStickDeadzone*100+.5f);
            int fullInput=int(state.xrMaxInputThreshold*100+.5f);
            if(widgets::SliderInt("Мёртвая зона левого стика",&leftDeadzone,0,30,"%d%%")){state.xrLeftStickDeadzone=leftDeadzone*.01f;changed=true;}
            if(widgets::SliderInt("Мёртвая зона правого стика",&rightDeadzone,0,30,"%d%%")){state.xrRightStickDeadzone=rightDeadzone*.01f;changed=true;}
            if(widgets::SliderInt("Порог полного отклонения стика",&fullInput,80,100,"%d%%")){state.xrMaxInputThreshold=fullInput*.01f;changed=true;}
            if(ImGui::IsItemHovered())ImGui::SetTooltip("Исходное отклонение стика, которое даёт полный ввод и активирует бег, рывок и приседание.\nДиапазон между мёртвой зоной каждого стика и этим порогом преобразуется в 0-100%%.\nУ сканера и пошагового поворота сохраняются прежние отдельные пороги.");
            const char* moveSrcNames[] = { "Игра (камера)", "Шлем (голова)", "Левая рука", "Правая рука" };
            int moveSrc = state.xrMovementSource;
            if (moveSrc < 0 || moveSrc > 3) moveSrc = state.xrMovementControl != 0 ? 1 : 0;
            if (widgets::Combo("Источник движения", &moveSrc, moveSrcNames, IM_ARRAYSIZE(moveSrcNames))) {
                state.xrMovementSource = moveSrc;
                state.xrMovementControl = moveSrc != 0 ? 1 : 0;
                changed = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Игра - левый стик ведёт туда, куда смотрит камера (как в оригинале).\nШлем - левый стик ведёт туда, куда повёрнут шлем.\nЛевая/Правая рука - ведёт туда, куда указывает выбранный контроллер.\nТранспорт всегда использует направление игры.");
            }

            ImGui::Separator();
            changed |= CheckboxInt("Плавание брассом", &state.xrBreaststrokeSwim);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("В воде вытяните обе руки вперёд, разведите в стороны и потяните назад.\nДвижение начинается при гребке назад. Плавные гребки дают обычную скорость; быстрые ускоряют.\nПлывите туда, куда смотрит шлем, включая вверх/вниз; стик не нужен.\nЧтобы всплыть, поднимите разведённые руки до груди и опустите обе вниз.\nПовторяйте гребки для движения. Отклоните левый стик назад, чтобы остановиться.");
            }
            ImGui::Separator();
            changed |= CheckboxInt("Лазание по лестницам руками", &state.xrLadderGripClimb);
            if(ImGui::IsItemHovered())ImGui::SetTooltip("Захватите боковую стойку или перекладину. Потяните руку вниз для подъёма,\nили переместите вверх для спуска. Отпустите захват для остановки. Таз и ноги удерживаются на лестнице штатной анимацией.");
            changed |= CheckboxInt("Автоматически завершать подъём у верха лестницы", &state.xrLadderAutoFinish);
            if(ImGui::IsItemHovered())ImGui::SetTooltip("После движения вверх возле вершины завершает подъём, даже если отпустить кнопки захвата.\nДля отмены отклоните стик или поднимите удерживающую руку вверх.");
            if(state.xrLadderAutoFinish)changed |= widgets::SliderFloat("Расстояние для завершения подъёма", &state.xrLadderFinishDistance,.2f,1.2f,"%.2f м");
            ImGui::Separator();
            ImGui::TextUnformatted("Повороты и взгляд вверх/вниз");
            changed |= CheckboxInt("Отключить вертикальный поворот стиком", &state.xrDisableMouseY);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Вкл.: подавляет вертикальный поворот от игры и правого стика; взглядом вверх/вниз управляет шлем.\nВыкл.: сохраняет управление вертикальным поворотом через правый стик.");
            }
            changed |= CheckboxInt("Пошаговый поворот", &state.xrSnapTurn);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Преобразует горизонтальное отклонение правого стика в повороты на фиксированный угол\nвместо плавного вращения. Помогает уменьшить укачивание.");
            }
            if (state.xrSnapTurn != 0) {
                changed |= widgets::SliderFloat("Шаг поворота", &state.xrSnapTurnAngleDeg, 10.0f, 90.0f, "%.0f°");
            }

            widgets::Section("КЛАССИЧЕСКАЯ СХЕМА КОНТРОЛЛЕРА");
            ImGui::TextWrapped("Для выбранного игрового режима классическая схема заменяет назначения порта полной стандартной схемой Xbox. Пространственные VR-взаимодействия по-прежнему перехватывают только те элементы управления, которые используют в данный момент.");
            changed |= CheckboxInt("Классическое управление пешком", &state.xrClassicOnFootControls);
            ImGui::Indent();
            ImGui::BeginDisabled(state.xrClassicOnFootControls == 0);
            changed |= CheckboxInt("Отключить бег по вертикальной оси левого стика", &state.xrClassicDisableLsSprint);
            changed |= CheckboxInt("Отключить рывок / приседание по вертикальной оси правого стика", &state.xrClassicDisableRsDashCrouch);
            ImGui::EndDisabled();
            ImGui::Unindent();

            changed |= CheckboxInt("Классическое управление транспортом", &state.xrClassicVehicleControls);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Использует обычную схему Xbox для транспорта вместо руления захватом руля,\nвыхода по удержанию X и особых действий курков при вождении.");
            }
            ImGui::Indent();
            ImGui::BeginDisabled(state.xrClassicVehicleControls == 0);
            changed |= CheckboxInt("Поменять курки и кнопки захвата в транспорте", &state.xrClassicSwapTriggersGrips);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Курки становятся LB / RB. Аналоговые кнопки захвата становятся LT / RT для тормоза и газа.");
            }
            ImGui::EndDisabled();
            ImGui::Unindent();

            changed |= CheckboxInt("Классическое управление сканером", &state.xrClassicScannerControls);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Сохраняет жест «рука к уху» для сканера, но при активном сканере использует\nобычную схему Xbox вместо специальных назначений порта для быстрых взломов.");
            }

            }
            if(section<0)ImGui::EndTabItem();
        }

        if (section>=0 ? section==4 : ImGui::BeginTabItem("СТЕРЕО")) {
            if(ImGui::CollapsingHeader("ПРОИЗВОДИТЕЛЬНОСТЬ",ImGuiTreeNodeFlags_DefaultOpen)) {
                int reflex=state.nvidiaReflex+1;
                if(widgets::Combo("NVIDIA Reflex",&reflex,"Как в игре\0Выкл. (выше FPS)\0Вкл. (меньше задержка)\0Вкл. + Boost\0")) {
                    state.nvidiaReflex=reflex-1;changed=true;
                }
                ImGui::TextWrapped("Выключение может повысить загрузку GPU и FPS, но увеличить задержку ввода.");
                const int applied=cvr::reflex::GetAppliedMode();
                const char* appliedName=applied==0?"Выкл.":applied==1?"Вкл.":applied==2?"Вкл. + Boost":"Ожидание рендеринга";
                ImGui::TextDisabled("Применено: %s",appliedName);
            }
            if (ImGui::CollapsingHeader("СТЕРЕОИЗОБРАЖЕНИЕ", ImGuiTreeNodeFlags_DefaultOpen)) {
                changed |= DrawFovControl(state);
        changed |= widgets::SliderFloat("Предсказание движения (мс)", &state.xrMotionPredictMs, 0.0f, 60.0f, "%.1f мс");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Предсказывает положение и поворот головы на указанное число миллисекунд вперёд\nпо скорости её движения, компенсируя задержку от рендеринга до показа. 0 = выкл.\nУвеличивайте, пока движение не станет отзывчивым, без чрезмерного опережения.");
        }
        changed |= widgets::SliderFloat("Множитель стереобазы", &state.xrStereoScale, 0.25f, 5.0f, "%.2fx");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Тонкая настройка автоматического расстояния между глазами (IPD).\n1,0 = естественное откалиброванное расстояние, согласованное с IPD шлема.\nПопробуйте 0,8-1,2 по ощущениям; 3-5x усиливает глубину и делает\nчередование глаз заметным на обычном мониторе для проверки.");
        }
        // ── World scale + honest IPD ──
        changed |= widgets::SliderFloat("Масштаб мира", &state.xrWorldScale, 0.20f, 3.0f, "%.2f");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Одновременно масштабирует расстояние между глазами И перемещение головы.\nУменьшите значение (например, до 0,8), чтобы мир казался БОЛЬШЕ,\nа вы - меньше; увеличьте, чтобы уменьшить мир. Используйте, если Ви\nи другие персонажи кажутся слишком крупными.");
        }
        changed |= widgets::SliderFloat("Множитель расстояния между глазами (IPD)", &state.xrIpdScale, 0.50f, 2.0f, "%.2fx");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Множитель межглазного расстояния (IPD), полученного от среды VR.\n1,0 = нейтральная база (±0,033 м для каждого глаза в типичном шлеме).\nВлияет на стереоскопическую глубину (эффект миниатюры или гигантского мира),\nа НЕ на размер изображения при просмотре одним глазом.");
        }
        bool reuseLastFrame = state.xrReuseLastFrame != 0;
        if (ImGui::Checkbox("Переиспользовать последний чистый кадр", &reuseLastFrame)) {
            state.xrReuseLastFrame = reuseLastFrame ? 1 : 0;
            changed = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Когда нового кадра нет, повторно отправляет последний корректный захваченный кадр глаза\nи позволяет композитору выполнить репроекцию вместо повторного преобразования старого\nизображения. Частота отправки может снизиться\nдо частоты захвата. Выкл.: всегда преобразовывать старый кадр глаза.");
        }
        // The "Pose pair-lock" checkbox is gone: it toggled a freeze that only meant something
        // under AER's one-camera eye alternation. The ini key still round-trips so old files load.
            }

            DrawStereoControls();
            if(section<0)ImGui::EndTabItem();
        }

        if (section>=0 ? section==5 : ImGui::BeginTabItem("АВАТАР")) {
            DrawVRHandsControls();
            if(section<0)ImGui::EndTabItem();
        }

        if(section<0)ImGui::EndTabBar();
    }

    return changed;
}

}  // namespace overlay
using namespace overlay;

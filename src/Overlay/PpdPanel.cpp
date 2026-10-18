#include "Overlay/PpdPanel.hpp"
#include "Overlay/VrWidgets.hpp"
#include "Framegen/Framegen.hpp"
#include "Framegen/NativeFgObservation.hpp"
#include "Framegen/SrNativeApi.hpp"
#include <Windows.h>
#include <imgui.h>
#include <mutex>
#include <cstring>
extern void Log(const char* fmt,...);

namespace cvr::ppd {
namespace {
std::mutex panelMutex;
PpdPanelController controller;
void PollLocked(uint64_t now,bool enabled){
    controller.SetAerEnabled(enabled,now);
    controller.ObserveNative(ReadNativeApplyStatus(),now);
}
const char* StageText(PpdPanelStage stage){
    switch(stage){
    case PpdPanelStage::Ready:return "Размер можно адаптировать";
    case PpdPanelStage::Requested:return "Запрос размера создан";
    case PpdPanelStage::WaitingNative:return "Ожидаем изменение размера в игре";
    case PpdPanelStage::WaitingSource:return "Ожидаем игровые кадры с подтверждёнными размерами";
    case PpdPanelStage::WaitingEyes:return "Ожидаем принятую подачу обоим глазам";
    case PpdPanelStage::Applied:return "Размер применён";
    case PpdPanelStage::Error:return "Размер не подтверждён";
    }
    return "Ожидаем данные";
}
const char* ErrorText(PpdPanelError error){
    switch(error){
    case PpdPanelError::UnsupportedMode:return "Проверка прервана при смене режима. Восстановление прежнего размера не подтверждено.";
    case PpdPanelError::InvalidSource:return "Данные игровых кадров не согласованы. Применение не подтверждено.";
    case PpdPanelError::ContextChanged:return "Сцена или система отсчёта изменились. Предыдущее подтверждение больше не действует.";
    case PpdPanelError::RequestRejected:return "Не удалось создать запрос размера.";
    case PpdPanelError::NativeFailure:return "Не удалось передать размер игре. Проверьте предпросмотр перед повтором.";
    case PpdPanelError::RevisionChanged:return "Запрос заменён другим запросом размера.";
    case PpdPanelError::Timeout:return "За 15 секунд размер не подтверждён всей цепочкой. Предыдущее значение не объявлено применённым.";
    case PpdPanelError::ClockChanged:return "Проверка времени прервана. Создайте новый запрос.";
    case PpdPanelError::SourceChanged:return "Ресурсы вывода изменились. Предыдущее подтверждение больше не действует.";
    default:return "";
    }
}
void RunDiagnosticSequenceLocked(uint64_t now){
    // Explicit simulator-only test of the SAME controller transaction as F10.
    // No input injection or native memory writes outside RequestNativeSize.
    struct Sequence {
        bool enabled=false,finished=false,waiting=false;
        unsigned step{};uint64_t due{},revision{};
        Sequence(){char value[32]{};const auto n=GetEnvironmentVariableA("CPVR_PPD_DIAGNOSTIC",value,sizeof(value));
            enabled=n&&n<sizeof(value)&&std::strcmp(value,"7.9,13,16")==0&&GetModuleHandleW(L"openxr_simulator.dll")!=nullptr;}
    };
    static Sequence s;
    if(!s.enabled||s.finished)return;
    const auto v=controller.Snapshot();
    if(s.waiting){
        if(v.stage==PpdPanelStage::Error){
            Log("[PPD-DIAGNOSTIC] FAIL step=%u revision=%llu error=%u\n",s.step,s.revision,unsigned(v.error));s.finished=true;return;
        }
        if(v.stage!=PpdPanelStage::Applied||v.nativeRevision!=s.revision)return;
        const auto& l=v.measuredSource.eyes[0];const auto& r=v.measuredSource.eyes[1];
        Log("[PPD-DIAGNOSTIC] APPLIED step=%u revision=%llu target=%.1f measured=%.6f source=%ux%u eyes=%ux%u/%ux%u\n",
            s.step,s.revision,v.appliedTarget,v.appliedMeasured,l.mainSource.width,l.mainSource.height,
            l.outputEye.width,l.outputEye.height,r.outputEye.width,r.outputEye.height);
        s.waiting=false;++s.step;s.due=now+5000;
        if(s.step==3){s.finished=true;Log("[PPD-DIAGNOSTIC] COMPLETE three accepted transitions; physical HMD untested\n");}
        return;
    }
    if(!v.calibrated||!v.aerV2)return;
    if(!s.due){s.due=now+5000;return;}
    if(now<s.due)return;
    constexpr float targets[]{7.9f,13.f,16.f};
    if(!controller.SetTarget(targets[s.step])||!controller.Apply(now,RequestNativeSize)){
        Log("[PPD-DIAGNOSTIC] FAIL to request step=%u\n",s.step);s.finished=true;return;
    }
    const auto next=controller.Snapshot();s.revision=next.nativeRevision;s.waiting=true;
    Log("[PPD-DIAGNOSTIC] REQUEST step=%u revision=%llu target=%.1f size=%ux%u\n",
        s.step,s.revision,targets[s.step],next.requested.width,next.requested.height);
}
}
void ObservePpdSource(const PpdSourceObservation& source){
    std::lock_guard lock(panelMutex);const auto now=GetTickCount64();
    PollLocked(now,true);controller.ObserveSource(source,now);
}
void ObservePpdAccepted(const PpdAcceptedObservation& accepted){
    std::lock_guard lock(panelMutex);const auto now=GetTickCount64();
    PollLocked(now,true);controller.ObserveAccepted(accepted,now);
    RunDiagnosticSequenceLocked(now);
}
void InvalidatePpdSourceContext(){
    std::lock_guard lock(panelMutex);controller.InvalidateContext();
    // Releasing native ownership is not a proven engine rollback. The mode
    // owner coordinates restoring author dimensions before enabling paired VR.
}
PpdPanelSnapshot ReadPpdPanelSnapshot(){
    std::lock_guard lock(panelMutex);const auto now=GetTickCount64();
    PollLocked(now,true);
    // Native FG can run without another captured-real acceptance for seconds.
    // Drive the simulator-only sequence from the regular controller poll, not
    // exclusively from the event which the pending transaction must request.
    RunDiagnosticSequenceLocked(now);
    return controller.Snapshot();
}
void DrawPpdPanel(){
    // This small UI transaction keeps the displayed preview and the Adapt
    // action on the same metadata snapshot. No GPU wait or per-frame I/O.
    std::lock_guard lock(panelMutex);const bool enabled=true;
    const auto now=GetTickCount64();PollLocked(now,enabled);
    auto view=controller.Snapshot();
    overlay::widgets::Section("Чёткость (PPD)");
    ImGui::TextUnformatted("PPD по фактической проекции");
    float target=view.target;
    if(overlay::widgets::SliderFloat("Целевой PPD",&target,5,40,"%.1f",ImGuiSliderFlags_AlwaysClamp))controller.SetTarget(target);
    view=controller.Snapshot();
    if(view.calibrated){
        ImGui::Text("Фактически: %.2f PPD",view.calibration.ppd);
        const auto& eyes=view.measuredSource.eyes;
        ImGui::Text("Игровой источник: %u × %u",eyes[0].mainSource.width,eyes[0].mainSource.height);
        ImGui::Text("Вход SR: Л %u × %u   П %u × %u",eyes[0].srInput.width,eyes[0].srInput.height,eyes[1].srInput.width,eyes[1].srInput.height);
        ImGui::Text("Выход глазам: Л %u × %u   П %u × %u",eyes[0].outputEye.width,eyes[0].outputEye.height,eyes[1].outputEye.width,eyes[1].outputEye.height);
    } else ImGui::TextDisabled("Калибровка появится после принятых игровых кадров обоих глаз.");
    if(view.preview.valid)ImGui::Text("Предпросмотр: %d × %d",view.preview.width,view.preview.height);
    else ImGui::TextDisabled("Предпросмотр недоступен");
    const bool nativeFgSession=cvr::framegen::NativeFgStereoRequested()&&cvr::framegen::NativeFgGameWasEnabled();
    ImGui::BeginDisabled(!view.canAdapt||nativeFgSession);
    if(ImGui::Button("Адаптировать"))controller.Apply(now,RequestNativeSize);
    ImGui::EndDisabled();view=controller.Snapshot();
    if(nativeFgSession)ImGui::TextWrapped("Тестовая сборка: для изменения PPD выключите генерацию в настройках игры и перезапустите игру. Смена размера вместе с FG ещё не готова.");
    if(view.sameQuantizedSize)ImGui::TextDisabled("Размер уже соответствует предпросмотру с округлением до 16 пикселей.");
    ImGui::TextUnformatted(StageText(view.stage));
    if(view.nativeRevision)ImGui::Text("Запрошено: %u × %u, цель %.1f PPD",view.requested.width,view.requested.height,view.requestedTarget);
    if(view.stage==PpdPanelStage::WaitingEyes)ImGui::Text("Подтверждено глаз: %u / 2",unsigned((view.acceptedEyeMask&1)!=0)+unsigned((view.acceptedEyeMask&2)!=0));
    if(view.stage==PpdPanelStage::Applied)ImGui::Text("При подтверждении: %.2f PPD (цель %.1f)",view.appliedMeasured,view.appliedTarget);
    if(view.error!=PpdPanelError::None)ImGui::TextWrapped("%s",ErrorText(view.error));
}
}

#include "Framegen/Presenter.hpp"
#include "Framegen/NativeGenerationBridge.hpp"
#include <algorithm>
#include <cmath>
#include <psapi.h>
extern void Log(const char*,...);

namespace cvr::framegen {
namespace {
bool ExternalBridgeLoaded() {
    static uint64_t nextCheck=0;static bool loaded=false;
    const auto now=GetTickCount64();if(now<nextCheck)return loaded;nextCheck=now+2000;loaded=false;
    HMODULE modules[1024]{};DWORD bytes{};
    if(EnumProcessModules(GetCurrentProcess(),modules,sizeof(modules),&bytes)) {
        const auto count=std::min<size_t>(bytes/sizeof(HMODULE),std::size(modules));
        for(size_t i=0;i<count;++i) {
            wchar_t name[MAX_PATH]{};GetModuleBaseNameW(GetCurrentProcess(),modules[i],name,MAX_PATH);
            if(_wcsnicmp(name,L"XR_APILAYER_XRFrameBridge",23)==0) {loaded=true;break;}
        }
    }
    return loaded;
}
XrPosef Middle(const XrPosef& a,const XrPosef& b) {
    XrPosef out{};out.position={(a.position.x+b.position.x)*.5f,(a.position.y+b.position.y)*.5f,(a.position.z+b.position.z)*.5f};
    const auto& x=a.orientation;const auto& y=b.orientation;
    const float sign=x.x*y.x+x.y*y.y+x.z*y.z+x.w*y.w<0 ? -1.0f : 1.0f;
    out.orientation={x.x+sign*y.x,x.y+sign*y.y,x.z+sign*y.z,x.w+sign*y.w};
    auto& q=out.orientation;const float norm=std::sqrt(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w);
    if(norm>0) {q.x/=norm;q.y/=norm;q.z/=norm;q.w/=norm;}else q={0,0,0,1};
    return out;
}
bool SameProjection(const Frame& a,const Frame& b) {
    for(unsigned eye=0;eye<2;++eye) {
        const auto& x=a.fov[eye];const auto& y=b.fov[eye];
        if(std::abs(x.angleLeft-y.angleLeft)>.001f || std::abs(x.angleRight-y.angleRight)>.001f ||
           std::abs(x.angleUp-y.angleUp)>.001f || std::abs(x.angleDown-y.angleDown)>.001f)return false;
        const auto& q=a.pose[eye].orientation;const auto& r=b.pose[eye].orientation;
        if(std::abs(q.x*r.x+q.y*r.y+q.z*r.z+q.w*r.w)<.95f)return false;
    }
    return true;
}
}
bool Presenter::SetNativeBridge(std::shared_ptr<NativeGenerationBridge> value) {
    if(nativeBridge) {
        nativeBridge->Collect();
        if(nativeBridge->Status().retainedConsumers)return false;
    }
    // This is an explicit owner change. After the old GPU consumers retire,
    // discard its queued CPU endpoint instead of waiting for a Select/Submitted
    // path that the new owner has already stopped calling.
    if(!generator.Reset())return false;
    Reset();nativeBridge=std::move(value);return true;
}
Selection Presenter::Select(ID3D12Device* device,ID3D12CommandQueue* queue,Frame current,bool gameplay,PresentationTime time) {
    const auto settings=GetSettings();
    if(nativeBridge) {
        // The author's fixed midpoint cadence/auto-pace must not silently
        // replace the mode owner's policy or NVIDIA multiplier.
        PacingReady(false);
        Selection result{current,cadence.Select(current.identity.serial,false,false),{},false};
        if(!gameplay || !settings.enabled) {nativeBridge->Reset();return result;}
        if(ExternalBridgeLoaded()) {nativeBridge->Reset();Status("NVIDIA: активен внешний XR frame bridge");return result;}
        if(nativeBridge->Select(device,queue,current,time,result)) {
            if(result.ownWitness.attempt)result.ownWitness.presenter=OwnPresenterPath::NativeSelected;
            Status(result.synthetic ? "Наш NVIDIA: выбран сгенерированный продукт" : "Наш NVIDIA: настоящий кадр",result.synthetic);
        } else {
            if(result.ownWitness.attempt){result.ownWitness.presenter=OwnPresenterPath::CurrentRealFallback;result.ownWitness.fallbackSerial=current.identity.serial;}
            const auto code=nativeBridge->Status().code;
            Status(code==NativeBridgeCode::IncompleteCamera ? "NVIDIA: отсутствуют полные матрицы камеры" :
                (code==NativeBridgeCode::MissingCameraTime || code==NativeBridgeCode::MissingPoseTime) ? "NVIDIA: отсутствует подтверждённое время камеры" :
                code==NativeBridgeCode::BackendPending ? "NVIDIA: модуль ещё не готов" : "NVIDIA: ожидание согласованного продукта");
        }
        if(result.ownWitness.attempt)nativeBridge->RecordOwnWitness(result.ownWitness);
        return result;
    }
    if(settings.backend==Backend::OwnNvidia && settings.enabled) {
        Reset();Status("Наш NVIDIA: ожидается подключение владельца режима");
        return {current,Cadence::Next::Real,{},false};
    }
    if(!gameplay || (pending.identity.serial && pending.identity.origin!=current.identity.origin))Reset();
    if(cadence.Pending())return {pending,Cadence::Next::PendingReal,{},false};
    Selection result{current,cadence.Select(current.identity.serial,false,false),{},false};
    if(!settings.enabled || !gameplay) {
        primed=false;previous={};generatedSerial=0;
        PacingReady(false);
        if(generator.Reset()) {ReleaseInputs();if(MetricsEnabled())SetVram(InputVram());}
        Status(settings.enabled ? "Пауза: меню или ракурс без отслеживания" : "Выключено");return result;
    }
    if(ExternalBridgeLoaded()) {
        Reset();Status("Загружен OFXR: выключите его в трее и перезапустите игру для встроенной генерации");return result;
    }
    if(!current.identity.inputs || !current.identity.stereo || !current.lease) {
        primed=false;PacingReady(false);Status("Ожидание согласованных глубины и векторов движения обоих глаз");return result;
    }
    if(current.identity.serial==generatedSerial)return result;
    const bool interpolate=primed && CanInterpolate(previous.identity,current.identity) && SameProjection(previous,current);
    static const bool historyDiagnostic=[] {char flag[8]{};return GetEnvironmentVariableA("CPVR_FG_HISTORY_DIAG",flag,sizeof(flag))==1&&flag[0]=='1';}();
    static uint64_t historySamples{};
    const auto historySample=historyDiagnostic?++historySamples:0;
    const bool reportHistory=historySample&&(historySample<=16||historySample%180==0);
    if(reportHistory)Log("[OWN-FG-HISTORY] sample=%llu primed=%d identity=%d projection=%d interpolate=%d serial=%llu/%llu native=%u/%u origin=%llu/%llu dt=%.6f stereo=%d/%d inputs=%d/%d poses=%llu,%llu/%llu,%llu reset=%d\n",
        historySample,int(primed),int(CanInterpolate(previous.identity,current.identity)),int(SameProjection(previous,current)),int(interpolate),
        previous.identity.serial,current.identity.serial,previous.identity.nativeFrame,current.identity.nativeFrame,previous.identity.origin,current.identity.origin,
        current.identity.capturedMs-previous.identity.capturedMs,int(previous.identity.stereo),int(current.identity.stereo),int(previous.identity.inputs),int(current.identity.inputs),
        previous.identity.pose[0],previous.identity.pose[1],current.identity.pose[0],current.identity.pose[1],int(current.identity.reset));
    const double milliseconds=interpolate ? current.identity.capturedMs-previous.identity.capturedMs : 1000.0/45;
    bool generated=false;
    if(!generator.Generate(device,queue,current.color,current.inputs,settings,milliseconds,!interpolate,++sdkFrame,&generated)) {
        primed=false;OnSkip();return result;
    }
    if(reportHistory)Log("[OWN-FG-HISTORY] sample=%llu generated=%d resetRequested=%d\n",historySample,int(generated),int(!interpolate));
    generatedSerial=current.identity.serial;
    PacingReady(true);
    if(interpolate && generated) {
        result.kind=Cadence::Next::Midpoint;result.synthetic=true;result.generatedFrom=current.lease;
        for(unsigned eye=0;eye<2;++eye) {
            result.frame.color[eye]=generator.Output(eye);
            result.frame.pose[eye]=Middle(previous.pose[eye],current.pose[eye]);
            result.frame.identity.pose[eye]=0; // synthetic pixels have a blended pose, not B's receipt
        }
        pending=current;
        Status("Генерация по векторам движения и глубине движка",true);
    } else {
        result.generatedFrom=current.lease;
        Status("Подготовка истории кадров",false);
    }
    // The SDK owns the previous-color history. Keep metadata only, not another
    // reference/pin/copy of the port's old capture buffers.
    previous=current;previous.lease.reset();previous.inputs[0].reset();previous.inputs[1].reset();
    previous.color[0]=previous.color[1]=nullptr;primed=true;
    return result;
}
void Presenter::Submitted(const Selection& result,ID3D12Fence* fence,uint64_t value,bool accepted,
    bool copySubmitted,bool signalSucceeded,int64_t displayTime) {
    if(result.native && nativeBridge) {
        nativeBridge->Submitted(result,fence,value,copySubmitted,signalSucceeded,accepted,displayTime);
        if(accepted && MetricsEnabled())OnSubmitted(result.frame.identity.serial,result.synthetic,result.kind==Cadence::Next::Repeat,NowMs());
        return;
    }
    if(nativeBridge&&result.ownWitness.attempt)nativeBridge->ObserveOwnFallback(result,accepted,copySubmitted,signalSucceeded,displayTime);
    if(result.frame.lease)result.frame.lease->fence=std::max(result.frame.lease->fence,value);
    if(result.generatedFrom)result.generatedFrom->fence=std::max(result.generatedFrom->fence,value);
    generator.ConsumerFence(fence,value);
    if(!accepted) {Reset("rejected-submit");return;}
    cadence.Accepted(result.kind,result.frame.identity.serial);
    if(MetricsEnabled())OnSubmitted(result.frame.identity.serial,result.synthetic,result.kind==Cadence::Next::Repeat,NowMs());
    if(result.kind==Cadence::Next::PendingReal)pending={};
}
void Presenter::Reset(const char* reason) {
    static const bool diagnostic=[] {char flag[8]{};return GetEnvironmentVariableA("CPVR_FG_HISTORY_DIAG",flag,sizeof(flag))==1&&flag[0]=='1';}();
    static uint64_t resets{};
    if(diagnostic){const auto n=++resets;if(n<=16||n%180==0)Log("[OWN-FG-RESET] count=%llu reason=%s primed=%d previous=%llu pending=%llu\n",n,reason,int(primed),previous.identity.serial,pending.identity.serial);}
    PacingReady(false);
    cadence.Reset();pending={};previous={};primed=false;generatedSerial=0;
    if(generator.Reset())ReleaseInputs();
    if(nativeBridge)nativeBridge->Reset();
}
}

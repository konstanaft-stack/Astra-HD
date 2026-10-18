#include "Framegen/SourceFrameSystem.hpp"
#include "Framegen/SourceSceneDelivery.hpp"
#include "Camera/PoseIdentity.hpp"
#include <RED4ext/RED4ext.hpp>
#include <RED4ext/GameEngine.hpp>
#include <RED4ext/RTTITypes.hpp>
#include <RED4ext/SystemUpdate.hpp>
#include <RED4ext/Scripting/Natives/gameIGameSystem.hpp>
#include <mutex>

namespace cvr::framegen {
namespace {
std::mutex stateMutex;
SourceSceneEpoch sceneClock;
SourceFrameSystemStatus diagnostics;
SourceSceneSink sink{};
std::shared_ptr<void> sinkContext;
uint64_t sinkRevision=1;

void Deliver(SourceSceneFrame scene,uint64_t revision,SourceSceneSink callback,void* context) noexcept {
    if(!callback)return;
    InvokeSourceSceneCallback([&]{callback(scene,context);},[&]{
        std::optional<MainEyeSourceFaultScope> revoke;
        {std::lock_guard lock(stateMutex);++diagnostics.callbackErrors;
         if(SourceSceneDeliveryCurrent({revision,scene},sinkRevision,sceneClock.Snapshot())) {
             revoke=camera::CaptureMainEyeSourceFaultScope(scene.worldEpoch,scene.ordinal);
             sceneClock.RejectFrame(scene);diagnostics.scene=sceneClock.Snapshot();
         }}
        // Revalidate request/latch/scene boundary inside the latch after
        // releasing stateMutex. An old in-flight callback cannot revoke a
        // newly published owner/frame while callback lifetime remains leased.
        if(revoke)camera::RevokeMainEyeSourceFault(*revoke);
    });
}
template<class F> void ChangeWorld(F action) {
    SourceSceneSink callback{};std::shared_ptr<void> context;SourceSceneFrame scene;uint64_t revision{};
    {std::lock_guard lock(stateMutex);action(sceneClock);scene=sceneClock.Snapshot();
     diagnostics.scene=scene;callback=sink;context=sinkContext;revision=sinkRevision;}
    Deliver(scene,revision,callback,context.get());
}

class SourceFrameSystem final:public RED4ext::game::IGameSystem {
public:
    using AllocatorType=RED4ext::Memory::ScriptAllocator;
    RED4ext::CClass* GetNativeType() override;
    void OnRegisterUpdates(RED4ext::UpdateRegistrar* registrar) override {
        if(!registrar)return;
        registrar->RegisterUpdate(RED4ext::UpdateTickGroup::FrameBegin,this,"CVR source frame begin",
            RED4ext::GroupUpdateCallback(this,&SourceFrameSystem::OnFrameBegin));
        std::lock_guard lock(stateMutex);++diagnostics.registrations;
    }
    void OnWorldAttached(RED4ext::world::RuntimeScene* scene) override {
        ChangeWorld([&](auto& clock){clock.Attach(reinterpret_cast<uintptr_t>(scene));});
    }
    void OnBeforeWorldDetach(RED4ext::world::RuntimeScene* scene) override {
        ChangeWorld([&](auto& clock){clock.Detach(reinterpret_cast<uintptr_t>(scene));});
    }
    void OnWorldDetached(RED4ext::world::RuntimeScene* scene) override {
        ChangeWorld([&](auto& clock){clock.Detach(reinterpret_cast<uintptr_t>(scene));});
    }
    void OnGamePaused() override {ChangeWorld([](auto& clock){clock.Pause(true);});}
    void OnGameResumed() override {ChangeWorld([](auto& clock){clock.Pause(false);});}
    void OnUninitialize() override {
        ChangeWorld([](auto& clock){clock.Shutdown();});
        std::lock_guard lock(stateMutex);diagnostics.installed=false;
    }
private:
    void OnFrameBegin(RED4ext::FrameInfo&,RED4ext::JobQueue&) {
        SourceSceneSink callback{};std::shared_ptr<void> context;SourceSceneFrame scene;uint64_t revision{};
        {std::lock_guard lock(stateMutex);scene=sceneClock.BeginFrame();diagnostics.scene=scene;
         ++diagnostics.frameBeginCalls;callback=sink;context=sinkContext;revision=sinkRevision;}
        Deliver(scene,revision,callback,context.get());
    }
};
RED4ext::TTypedClass<SourceFrameSystem> sourceSystemType("CVRAerSourceFrameSystem");
RED4ext::CClass* SourceFrameSystem::GetNativeType(){return &sourceSystemType;}

void RegisterType() {
    RED4ext::CNamePool::Add("CVRAerSourceFrameSystem");
    sourceSystemType.flags={.isNative=true};
    RED4ext::CRTTISystem::Get()->RegisterType(&sourceSystemType);
}
void DescribeAndInstall() {
    auto* rtti=RED4ext::CRTTISystem::Get();
    sourceSystemType.parent=rtti->GetClass("gameIGameSystem");
    if(!sourceSystemType.parent)return;
    auto* engine=RED4ext::CGameEngine::Get();
    if(!engine||!engine->framework||!engine->framework->gameInstance) {
        rtti->AddPostRegisterCallback(DescribeAndInstall);return;
    }
    auto* game=engine->framework->gameInstance;
    if(game->systemMap.Get(&sourceSystemType))return;
    // Registration sequence follows the MIT-licensed RedLib SystemBuilder.
    // See third_party/redlib-system-registration.LICENSE.txt and the pinned
    // reference in audit/framebegin-owner-20261004/redlib-reference.
    auto instance=RED4ext::MakeHandle<SourceFrameSystem>();
    if(!instance)return;
    RED4ext::JobHandle job{};instance->gameInstance=game;instance->OnInitialize(job);
    game->systemMap.Insert(&sourceSystemType,instance);
    game->systemImplementations.Insert(&sourceSystemType,&sourceSystemType);
    game->systemInstances.EmplaceBack(instance);
    std::lock_guard lock(stateMutex);diagnostics.installed=true;
}
}
void RegisterSourceFrameSystem() {
    static std::once_flag once;
    std::call_once(once,[]{auto* rtti=RED4ext::CRTTISystem::Get();
        rtti->AddRegisterCallback(RegisterType);rtti->AddPostRegisterCallback(DescribeAndInstall);});
}
void SetSourceSceneSink(SourceSceneSink callback,std::shared_ptr<void> context) {
    std::shared_ptr<void> previous;
    {std::lock_guard lock(stateMutex);
     if(sinkRevision==(std::numeric_limits<uint64_t>::max)())return;
     ++sinkRevision;sink=callback;previous=std::move(sinkContext);sinkContext=std::move(context);}
}
SourceFrameSystemStatus ReadSourceFrameSystemStatus() {
    std::lock_guard lock(stateMutex);return diagnostics;
}
}

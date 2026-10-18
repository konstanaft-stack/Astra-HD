// Same real production hook TU and mocks as the established native-hook suite.
// The RED adapter below does nothing when the new factory API is absent: it
// supplies no replacement lifecycle logic and exposes the old permanent latch.
#define main ExistingNativeHookTests
#include "sr_native_hooks.cpp"
#undef main

static unsigned factoryCalls{},factoryEmpty{},factoryThrows{};
static uint64_t factoryGeneration{};
static std::shared_ptr<Owner> nextOwner;
static bool factoryReenters{};
static std::shared_ptr<SrNativeHandler> Factory(uint64_t generation) {
    ++factoryCalls;factoryGeneration=generation;
    CHECK(GetSrNativeHookReadiness().quiescing);
    CHECK(EnsureSrNativeHooks()); // no installer/metadata lock held by callback
    if(factoryReenters)CHECK(test::Shutdown()==sl::Result::eErrorInvalidState);
    if(factoryThrows){--factoryThrows;throw std::runtime_error("factory failure");}
    if(factoryEmpty){--factoryEmpty;return {};}
    return nextOwner;
}
template<class F>static void RegisterFactory(F factory) {
    if constexpr(requires(F f){SetSrNativeLifecycleFactory(f);})SetSrNativeLifecycleFactory(factory);
    else (void)factory;
}
static void LifecycleReset() {
    Reset();Ready();factoryCalls=factoryEmpty=factoryThrows=0;factoryGeneration=0;
    factoryReenters=false;nextOwner=std::make_shared<Owner>();RegisterFactory(&Factory);
}
static void RearmBeforeOriginalInit() {
    LifecycleReset();auto previous=std::make_shared<Owner>();CHECK(SetSrNativeHandler(previous));
    const auto before=GetSrNativeHookReadiness().lifecycleGeneration;
    CHECK(test::Shutdown()==sl::Result::eOk);CHECK(GetSrNativeHookReadiness().quiescing);
    Params params;NVSDK_NGX_Handle* handle{};sl::Preferences prefs;
    mock.duringInit=[&]{CHECK(factoryCalls==1);CHECK(!GetSrNativeHookReadiness().quiescing);
        CHECK(test::Create(nullptr,NVSDK_NGX_Feature_SuperSampling,&params,&handle)==NVSDK_NGX_Result_Success);};
    CHECK(test::Init(prefs,321)==sl::Result::eOk);
    CHECK(factoryCalls==1&&factoryGeneration==before+1);
    CHECK(nextOwner->creates==1&&previous->creates==0&&mock.creates==1);
    test::Evaluate(nullptr,handle,&params,nullptr);
    CHECK(nextOwner->evaluates==1&&nextOwner->last.lifecycleGeneration==factoryGeneration);
    CHECK(params.touches==0&&mock.inits==1&&mock.shutdowns==1);
    mock.duringInit={};test::Init(prefs,321);CHECK(factoryCalls==1);
    CHECK(CyberpunkVR_SrNativeHookStatus.hostNgxInitProven==0);
}
static void RefusalAndOriginalFailureDoNotRearm() {
    for(unsigned scenario=0;scenario<3;++scenario){
        LifecycleReset();auto previous=std::make_shared<Owner>();CHECK(SetSrNativeHandler(previous));
        const auto life=GetSrNativeHookReadiness().lifecycleGeneration;
        if(scenario==0)previous->retireResult=SrNativeRetireResult::NotReady;
        if(scenario==1)mock.slResult=static_cast<sl::Result>(77);
        if(scenario==2)mock.throwShutdown=true;
        bool threw=false;try{const auto result=test::Shutdown();CHECK(result==(scenario?mock.slResult:sl::Result::eErrorInvalidState));}
        catch(const std::runtime_error&){threw=true;}
        CHECK(threw==(scenario==2));CHECK(mock.shutdowns==(scenario?1u:0u));
        CHECK(GetSrNativeHookReadiness().lifecycleGeneration==life);
        mock.throwShutdown=false;mock.slResult=sl::Result::eOk;sl::Preferences prefs;
        CHECK(test::Init(prefs,1)==sl::Result::eOk);CHECK(test::SetDevice(nullptr)==sl::Result::eOk);
        CHECK(factoryCalls==0&&GetSrNativeHookReadiness().quiescing&&!SetSrNativeHandler(nextOwner));
        CHECK(previous->retires==1);
    }
}
static void RetryFactoryBeforeDeviceAndAllocate() {
    for(unsigned boundary=0;boundary<2;++boundary){
        LifecycleReset();auto previous=std::make_shared<Owner>();CHECK(SetSrNativeHandler(previous));
        CHECK(test::Shutdown()==sl::Result::eOk);factoryEmpty=1;sl::Preferences prefs;
        CHECK(test::Init(prefs,1)==sl::Result::eOk);CHECK(factoryCalls==1&&GetSrNativeHookReadiness().quiescing);
        Params params;NVSDK_NGX_Handle* handle{};auto create=[&]{
            CHECK(factoryCalls==2);test::Create(nullptr,NVSDK_NGX_Feature_RayReconstruction,&params,&handle);};
        if(boundary==0){mock.duringDevice=create;CHECK(test::SetDevice(nullptr)==sl::Result::eOk);}
        else{mock.duringAllocate=create;sl::ViewportHandle viewport(0);CHECK(test::Allocate(nullptr,1001,viewport)==sl::Result::eOk);}
        CHECK(nextOwner->creates==1&&previous->creates==0&&nextOwner->last.featureId==13);
        CHECK(!GetSrNativeHookReadiness().quiescing&&params.touches==0);
    }
}
static void NoLateEvaluateFactoryAndUnknownOwnerPin() {
    LifecycleReset();auto previous=std::make_shared<Owner>();CHECK(SetSrNativeHandler(previous));
    CHECK(test::Shutdown()==sl::Result::eOk);factoryThrows=1;sl::Preferences prefs;
    CHECK(test::Init(prefs,1)==sl::Result::eOk);CHECK(factoryCalls==1&&GetSrNativeHookReadiness().quiescing);
    Params params;test::Evaluate(nullptr,&mock.handles[99],&params,nullptr);
    CHECK(factoryCalls==1&&nextOwner->creates==0&&mock.creates==0);
    NVSDK_NGX_Handle* handle{};test::Create(nullptr,NVSDK_NGX_Feature_SuperSampling,&params,&handle);
    CHECK(factoryCalls==2&&nextOwner->creates==1&&mock.creates==1); // real Create retry is allowed
    // A failed/unknown retirement may never be displaced by a fresh factory.
    LifecycleReset();previous=std::make_shared<Owner>();previous->retireResult=SrNativeRetireResult::NotReady;
    CHECK(SetSrNativeHandler(previous));std::weak_ptr<Owner> old=previous;previous.reset();
    CHECK(test::Shutdown()==sl::Result::eErrorInvalidState);test::Init(prefs,1);
    CHECK(!old.expired()&&factoryCalls==0);
}
static void SameRetiredOwnerIsNotFresh() {
    LifecycleReset();CHECK(SetSrNativeHandler(nextOwner));CHECK(test::Shutdown()==sl::Result::eOk);
    sl::Preferences prefs;test::Init(prefs,1);CHECK(factoryCalls==1&&GetSrNativeHookReadiness().quiescing);
}
static void FactoryRunsOnlyAfterRetiredLifecycle() {
    LifecycleReset();auto initial=std::make_shared<Owner>();CHECK(SetSrNativeHandler(initial));sl::Preferences prefs;
    test::Init(prefs,1);CHECK(factoryCalls==0);
    CHECK(test::Shutdown()==sl::Result::eOk);factoryReenters=true;test::Init(prefs,1);
    CHECK(factoryCalls==1&&mock.shutdowns==1&&!GetSrNativeHookReadiness().quiescing);
}
static void ShutdownInsideInitThenActualCreate() {
    LifecycleReset();auto previous=std::make_shared<Owner>();CHECK(SetSrNativeHandler(previous));
    Params params;NVSDK_NGX_Handle* handle{};sl::Preferences prefs;
    mock.duringInit=[&]{CHECK(test::Shutdown()==sl::Result::eOk);CHECK(factoryCalls==0);
        test::Create(nullptr,NVSDK_NGX_Feature_SuperSampling,&params,&handle);
        CHECK(factoryCalls==1&&nextOwner->creates==1&&previous->creates==0);};
    CHECK(test::Init(prefs,123)==sl::Result::eOk);test::Evaluate(nullptr,handle,&params,nullptr);
    CHECK(nextOwner->evaluates==1&&nextOwner->last.lifecycleGeneration==factoryGeneration);
    CHECK(mock.inits==1&&mock.shutdowns==1&&mock.creates==1&&params.touches==0);
    const auto& wire=CyberpunkVR_SrNativeLifecycleStatus;
    uint64_t shutdownEnd=0,rearmEnd=0,createBegin=0,initEnd=0;
    for(const auto&e:wire.events){
        if(e.event==uint32_t(SrLifecycleEvent::ShutdownOriginalEnd)){shutdownEnd=e.serial;CHECK(e.result==0&&(e.flags&SrLifecycleResultKnown));}
        if(e.event==uint32_t(SrLifecycleEvent::RearmEnd)&&e.result==uint32_t(SrRearmResult::Installed))rearmEnd=e.serial;
        if(e.event==uint32_t(SrLifecycleEvent::CreateBegin)){createBegin=e.serial;CHECK(e.lifecycleGeneration==factoryGeneration);}
        if(e.event==uint32_t(SrLifecycleEvent::InitEnd))initEnd=e.serial;
    }
    CHECK(shutdownEnd&&shutdownEnd<rearmEnd&&rearmEnd<createBegin&&createBegin<initEnd);
}
static void NoFactoryAndFailedInitKeepExactForwarding() {
    LifecycleReset();auto previous=std::make_shared<Owner>();CHECK(SetSrNativeHandler(previous));test::Shutdown();
    RegisterFactory(static_cast<std::shared_ptr<SrNativeHandler>(*)(uint64_t)>(nullptr));sl::Preferences prefs;
    test::Init(prefs,1);Params params;NVSDK_NGX_Handle*handle{};test::Create(nullptr,NVSDK_NGX_Feature_SuperSampling,&params,&handle);
    CHECK(GetSrNativeHookReadiness().quiescing&&previous->creates==0&&factoryCalls==0&&mock.creates==1);
    RegisterFactory(&Factory);mock.slResult=static_cast<sl::Result>(77);
    mock.duringInit=[&]{test::Create(nullptr,NVSDK_NGX_Feature_SuperSampling,&params,&handle);};
    CHECK(test::Init(prefs,2)==static_cast<sl::Result>(77));
    CHECK(factoryCalls==1&&nextOwner->creates==1&&mock.creates==2&&mock.inits==2);
    test::Evaluate(nullptr,handle,&params,nullptr);CHECK(nextOwner->evaluates==1&&params.touches==0);
}
static void BoundedLifecycleEvidence() {
    LifecycleReset();sl::Preferences prefs;for(unsigned i=0;i<80;++i)test::Init(prefs,i);
    const auto&wire=CyberpunkVR_SrNativeLifecycleStatus;CHECK(wire.version==1&&wire.bytes==4128&&wire.capacity==64&&wire.recordBytes==64);
    CHECK(!(CyberpunkVR_SrNativeLifecycleStatusSeq.load()&1)&&wire.publishedEvents==160&&wire.droppedEvents==0);
    for(uint64_t i=wire.publishedEvents-wire.Capacity+1;i<=wire.publishedEvents;++i){const auto&e=wire.events[(i-1)%wire.Capacity];
        CHECK(e.serial==i&&e.qpc&&e.lifecycleGeneration==1);CHECK(e.event==uint32_t(i&1?SrLifecycleEvent::InitBegin:SrLifecycleEvent::InitEnd));
        CHECK(bool(e.flags&SrLifecycleResultKnown)==!(i&1));}
    auto previous=std::make_shared<Owner>();CHECK(SetSrNativeHandler(previous));mock.throwShutdown=true;
    bool threw=false;try{test::Shutdown();}catch(const std::runtime_error&){threw=true;}
    CHECK(threw);const auto&e=wire.events[(wire.publishedEvents-1)%wire.Capacity];
    CHECK(e.event==uint32_t(SrLifecycleEvent::ShutdownOriginalBegin)&&!(e.flags&SrLifecycleResultKnown));
    FILE* file{};CHECK(fopen_s(&file,"sr-lifecycle-wire.bin","wb")==0&&file);
    if(file){CHECK(std::fwrite(&wire,1,sizeof(wire),file)==sizeof(wire));std::fclose(file);}
}
int main(){ExistingNativeHookTests();RearmBeforeOriginalInit();RefusalAndOriginalFailureDoNotRearm();
    RetryFactoryBeforeDeviceAndAllocate();NoLateEvaluateFactoryAndUnknownOwnerPin();SameRetiredOwnerIsNotFresh();FactoryRunsOnlyAfterRetiredLifecycle();
    ShutdownInsideInitThenActualCreate();NoFactoryAndFailedInitKeepExactForwarding();BoundedLifecycleEvidence();
    RegisterFactory(static_cast<std::shared_ptr<SrNativeHandler>(*)(uint64_t)>(nullptr));SetSrNativeHandler({});
    std::printf("SR lifecycle CPU: checks=%u failures=%u game/GPU/actual-hooks=not-used\n",checks,failures);return failures?1:0;
}

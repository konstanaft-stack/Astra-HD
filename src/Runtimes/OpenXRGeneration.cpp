#include "Runtimes/OpenXRManager.hpp"
#include "Framegen/OwnedGenerationPolicy.hpp"
#include <filesystem>

extern void Log(const char*,...);
namespace {
std::optional<cpvr::GenerationConfig> OwnedConfig(const cpvr::GenerationPolicy& policy) {
    HMODULE module{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&OwnedConfig),&module))return {};
    const auto core=GetModuleHandleW(L"_nvngx.dll");
    if(!core)return {}; // Retry after the game's NGX owner has initialized.
    std::array<wchar_t,32768> modulePath{},exePath{},corePath{};
    const auto m=GetModuleFileNameW(module,modulePath.data(),DWORD(modulePath.size()));
    const auto e=GetModuleFileNameW(nullptr,exePath.data(),DWORD(exePath.size()));
    const auto c=GetModuleFileNameW(core,corePath.data(),DWORD(corePath.size()));
    if(!m || m>=modulePath.size() || !e || e>=exePath.size() || !c || c>=corePath.size())return {};
    if(policy.generated_per_interval!=1){
        cvr::framegen::Status("Текущий комплект NVIDIA проверен для X2; выводятся настоящие кадры");
        return {};
    }
    cpvr::GenerationConfig config;
    config.backend_library=(std::filesystem::path(modulePath.data()).parent_path()/L"cpvr_nv_backend.dll").wstring();
    config.snippet_directory=std::filesystem::path(exePath.data()).parent_path().wstring();
    // Qualified 617.42 / 310.9.16 native-X2 tuple. Resolve the driver folder
    // from the host's loaded core, not a directory left by an older driver.
    // The backend verifies the wrapper, core and every loaded snippet copy.
    config.driver_library=(std::filesystem::path(corePath.data()).parent_path()/L"nvngx.dll").wstring();
    config.driver_sha256="B79D0ED1B677E9813476F91A447DA443D1897117BC4FF47F6D69FBDC618D6CEE";
    config.snippet_sha256="CADEE1AA6DB3907BABDC3285DA2AEBC4553D92B89C38BA3EAE8ABD8BBA1BEA72";
    config.policy=policy;config.enable_verified_compatibility_gate=false;
    // The game already owns NGX. Retain the verified worker's host-lifetime
    // policy; never add another game's hook owner or global slShutdown.
    config.initialize_ngx=false;
    return config;
}
}
bool OpenXRManager::UpdateOwnedGeneration(const cvr::framegen::Settings& settings) {
    using Retirement=cvr::framegen::RetirementState;
    const auto retiring=m_ownRetirement.Poll();
    if(retiring==Retirement::Pending){cvr::framegen::Status("Освобождение ресурсов NVIDIA; пока выводятся настоящие кадры");return false;}
    if(retiring==Retirement::Failed){cvr::framegen::Status("NVIDIA: безопасное освобождение не подтверждено; перезапустите игру");return false;}
    if(retiring==Retirement::Complete){
        m_ownGeneration.reset();m_ownGenerationPolicy.reset();m_ownRetirement.Reset();m_ownGenerationStopping=false;
        Log("[own NVIDIA] stopped cleanly; private worker/resources released\n");
    }
    if(m_ownGenerationStopping || !settings.enabled || settings.backend!=cvr::framegen::Backend::OwnNvidia) {
        if(m_ownGenerationConnected){
            if(!m_framegen.SetNativeBridge({})){cvr::framegen::Status("NVIDIA: ожидание завершения чтения кадров GPU");return false;}
            m_ownGenerationConnected=false;
        }
        if(m_ownGeneration){
            m_ownGenerationStopping=true; // Sticky across launch failure or an immediate UI re-enable.
            m_ownGenerationBridge.reset();m_ownGenerationPolicy.reset();
            m_ownAerSourceGeneration=m_ownAerModeGeneration=0;
            auto* worker=m_ownGeneration.get();
            if(m_ownRetirement.Begin([worker]{worker->stop();return worker->stopped_cleanly();}))
                Log("[own NVIDIA] asynchronous stop requested after consumer retirement\n");
            else cvr::framegen::Status("NVIDIA: не удалось начать освобождение ресурсов; повторяем");
            return false;
        }
        return true;
    }
    const auto policy=cvr::framegen::MakeOwnedGenerationPolicy(settings);
    if(!m_ownGeneration) {
        const auto config=OwnedConfig(policy);if(!config)return false;
        m_ownGeneration=std::make_unique<cpvr::GenerationWorker>();
        m_ownGenerationBridge=std::make_shared<cvr::framegen::NativeGenerationBridge>(*m_ownGeneration,*config,
            [](const std::string& message){Log("[own NVIDIA] %s\n",message.c_str());});
        m_ownGenerationPolicy.reset(); // Deferred start has not applied a policy yet.
    }
    if(!m_ownGenerationPolicy || *m_ownGenerationPolicy!=policy) {
        if(m_ownGeneration->configure(policy))m_ownGenerationPolicy=policy;
    }
    if(!m_ownGenerationConnected)m_ownGenerationConnected=m_framegen.SetNativeBridge(m_ownGenerationBridge);
    return m_ownGenerationConnected;
}

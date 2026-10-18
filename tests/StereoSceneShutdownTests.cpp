#include "Render/StereoSceneState.hpp"
#include "MenuPreview/UiConsumer.hpp"
#include <windows.h>
#include <cstdio>

static auto* const listKey=reinterpret_cast<ID3D12GraphicsCommandList*>(0x10000);
static auto* const pipelineKey=reinterpret_cast<ID3D12PipelineState*>(0x20000);
static void RetireAfterStaticCleanup()noexcept {
    __try {
        cvr::stereo::scene_state::Retire(listKey);
        cvr::stereo::scene_state::ResetCapture();
        if(!cvr::stereo::scene_state::Compatible(pipelineKey)){
            std::puts("FAIL: late release lost the still-addressable weak scene metadata");std::fflush(stdout);ExitProcess(14);
        }
        std::puts("PASS: actual scene-state retirement after CRT global cleanup");
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        std::printf("FAIL: late scene-state callback raised SEH %08lX\n",GetExceptionCode());std::fflush(stdout);ExitProcess(13);
    }
}
// Initialized before the production globals below, therefore destroyed AFTER
// them. This reproduces D3D12 private-data Lifetime::Release during teardown.
struct LateD3dRelease {
    LateD3dRelease(){SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);}
    ~LateD3dRelease(){RetireAfterStaticCleanup();}
} lateRelease;

#include "../src/Render/StereoSceneState.cpp"

extern "C" {std::atomic<uint32_t> CyberpunkVR_NativeStereoProbeState{1};}
namespace cvr::menu_preview {
bool UiConsumerEnabled()noexcept{return false;}
bool TryAcquireCurrent(Lease&)noexcept{return false;}
}
namespace cvr::stereo::native_probe {bool Enabled(){return false;}}

int main(){
    for(uintptr_t i=0;i<256;++i)cvr::stereo::scene_state::Reset(reinterpret_cast<ID3D12GraphicsCommandList*>(0x10000+i*16));
    cvr::stereo::scene_state::PipelineCreated(pipelineKey,true);
    std::puts("Production weak metadata populated; returning through real CRT teardown.");
    std::fflush(stdout);
}

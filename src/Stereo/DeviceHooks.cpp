#include "MenuPreview/UiConsumer.hpp"
#include "MenuPreview/PipelineEvidence.hpp"
#include "MenuPreview/DescriptorProvenance.hpp"
#include "MenuPreview/PixelReadback.hpp"
#include "Utils/DebugGate.hpp"
#include "Framegen/SrHistoryIntegration.hpp"
#include "Hooks/AftermathDiagnostics.hpp"
#include "Hooks/GpuObjectProvenance.hpp"
#include "Render/CommandResources.hpp"
#include "Render/NativeObjectIdentity.hpp"
#include "Render/NativeColorSource.hpp"
#include "MenuPreview/ImageLineage.hpp"
#include "Framegen/NativeFgReadback.hpp"
#include "Framegen/NativeFgOutput.hpp"
#include "Render/ResourceAllocation.hpp"
#include "Render/NativeStereoProbe.hpp"
#include "Render/StereoGpuProbe.hpp"
#include "Render/StereoSceneState.hpp"
#include "Render/RenderProbeScope.hpp"
#include <wrl/client.h>
// DeviceHooks -- how everything else in this module gets to run at all.
//
// The engine creates a D3D12 device, a queue, command lists and pipeline states. This file hooks the
// creation of each, so that by the time any other file here needs a command list to be watched, it
// already is.
//
// patch_command_list_vtable IS THE COUPLING THIS MODULE HAS NOT YET BROKEN. It installs TWELVE slots
// whose hooks are defined in three other files -- counted, not estimated:
//
//   CommandListCensus.cpp   7   slots 12, 13, 14, 15, 25, 44, 59  (draw, dispatch, copy, PSO, VB)
//   Dlss.cpp                3   slots 10, 21, 22                  (reset, viewport, scissor)
//   Capture.cpp             2   slots 26, 46                      (barrier, render targets)
//
// A vtable slot has no registry, so this function has to name every one of them -- precisely the
// coupling CVR_DETOUR removed for the RVA detours. A slot registry is the obvious next move; until it
// exists those hooks are declared in Stereo/StereoInternal.hpp and named here.
//
// (The mirror's appended copy is NOT in that list. It is called from Hook_ExecuteCommandLists, not
// installed into a slot, which is why it needed no declaration when the mirror moved out.)
//
// WHY THE DESCRIPTOR HEAP IS RESIZED. patch_descriptor_heap_size and Hook_CreateDescriptorHeap exist
// because the second view needs descriptors the engine did not budget for. Growing the heap at creation
// is the only place it can be done without owning the allocator.
//
// The pipeline-state hooks are not decoration either: identifying a draw by its PIXEL SHADER is what
// made the outline's blend mode findable, and pso_stream_find is how a shader is recognised inside a
// stream description whose layout the SDK does not expose.
//
// Everything here is a creation-time hook, so all of it runs before the frame loop and none of it is
// on a per-frame path. That is the reason this file can afford to log as much as it does.

#include "Stereo/SyncStereo.hpp"
#include "Render/WorldMarkerShader.hpp"
#include "Framegen/Inputs.hpp"
#include "Framegen/GpuTimer.hpp"
#include "Camera/ImagePoseIdentity.hpp"
#include "Utils/StereoLog.hpp"
#include "Stereo/VrcamConfig.hpp"   // vrcam.json access + CName hashing, shared with the launcher
#include "Render/ColorBlit.hpp"   // HUD debug overlay on the mirror image
#include <windows.h>
#include <d3d12.h>
#include "Hooks/SwapChainInternal.hpp"   // g_dredDevice -- the device, taken from the swapchain   // ID3D12InfoQueue, for the drain below
#include <d3d11.h>
#include <d3d11on12.h>
#include <dxgi1_4.h>
#include <intrin.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include "MinHook.h"
#include "Utils/LogThrottle.hpp"
#include "Stereo/StereoInternal.hpp"
#include "Stereo/EngineRvas.hpp"
#include "Stereo/CetOverlayLayer.hpp"
#include "Stereo/DetourRegistry.hpp"
#include "Stereo/StereoInternal.hpp"
#include "Stereo/EngineRvas.hpp"

namespace cvr {
namespace detail {

void patch_descriptor_heap_size() {
    if (!g_enable_desc_heap_resize) return;
    bool expected = false;
    if (!g_desc_heap_resized.compare_exchange_strong(expected, true)) return;
    if (!g_exe_base) sync_stereo_init();
    uint8_t* mov = g_exe_base + DESC_HEAP_SIZE_MOV_RVA;
    __try {
        if (mov[0] != 0xB8 ||
            *reinterpret_cast<uint32_t*>(mov + 1) != DESC_HEAP_SIZE_ORIG) {
            CyberpunkVR_DebugDescHeapResized = 0xFFFFFFFFu; // validation failed
            log("[descheap] size-const validation FAILED at %p (op=0x%02X imm=0x%X)",
                mov, mov[0], *reinterpret_cast<uint32_t*>(mov + 1));
            return;
        }
        DWORD oldp = 0;
        if (!VirtualProtect(mov + 1, 4, PAGE_EXECUTE_READWRITE, &oldp)) return;
        *reinterpret_cast<uint32_t*>(mov + 1) = DESC_HEAP_SIZE_NEW;
        DWORD junk = 0;
        VirtualProtect(mov + 1, 4, oldp, &junk);
        FlushInstructionCache(GetCurrentProcess(), mov, 5);
        CyberpunkVR_DebugDescHeapResized = DESC_HEAP_SIZE_NEW;
        log("[descheap] heap size const patched 0x%X -> 0x%X at %p",
            DESC_HEAP_SIZE_ORIG, DESC_HEAP_SIZE_NEW, mov);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        CyberpunkVR_DebugDescHeapResized = 0xEEEEEEEEu;
    }
}

static HRESULT STDMETHODCALLTYPE Hook_CreateDescriptorHeap(
        ID3D12Device* self, const D3D12_DESCRIPTOR_HEAP_DESC* desc,
        REFIID riid, void** out) {
    const uintptr_t ret_abs = reinterpret_cast<uintptr_t>(_ReturnAddress());
    CVR_DIAGNOSTIC(CyberpunkVR_DebugDescHeapCreates++);
    D3D12_DESCRIPTOR_HEAP_DESC local;
    const D3D12_DESCRIPTOR_HEAP_DESC* use = desc;
    if (desc) {
        const bool shader_visible =
            (desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0;
        if (desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
            shader_visible && desc->NumDescriptors >= 0x40000u) {
            uintptr_t rva = ret_abs - reinterpret_cast<uintptr_t>(g_exe_base);
            CyberpunkVR_DebugDescHeapSVNum = desc->NumDescriptors;
            CyberpunkVR_DebugDescHeapSVRetRva = rva;
            CyberpunkVR_DebugDescHeapSVRetAbs = ret_abs;
            CyberpunkVR_DebugDescHeapSVFlags = desc->Flags;
            log("[descheap] shader-visible CBV_SRV_UAV num=%u flags=0x%X caller_abs=0x%llX rva=0x%llX",
                desc->NumDescriptors, desc->Flags,
                (unsigned long long)ret_abs, (unsigned long long)rva);
            if (g_enable_desc_heap_enlarge && desc->NumDescriptors < g_desc_heap_target) {
                local = *desc;
                local.NumDescriptors = g_desc_heap_target;
                use = &local;
                CVR_DIAGNOSTIC(CyberpunkVR_DebugDescHeapEnlarged++);
                log("[descheap] enlarged -> %u", g_desc_heap_target);
            }
        }
    }
    const auto result=g_orig_CreateDescriptorHeap(self, use, riid, out);
    if(SUCCEEDED(result)&&out&&*out)cvr::menu_preview::PreviewDescriptorHeapCreated(self,static_cast<IUnknown*>(*out));
    if(SUCCEEDED(result)&&out&&*out&&cvr::menu_preview::UiStereoRequested()){
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
        if(SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&heap))))
            cvr::menu_preview::UiDescriptorHeapCreated(self,heap.Get());
    }
    if(SUCCEEDED(result)&&out&&*out)NameGpuObjectProvenance(static_cast<IUnknown*>(*out),L"DescriptorHeap",reinterpret_cast<const void*>(ret_abs));
    return result;
}

// --- ExecuteCommandLists probe: count actual GPU command-list executions per
// frame. Definitive test for "does the eye execute?": if execLists/frame ~doubles
// under stereo, the eye's command lists really run on the GPU. Queue vtable slot
// 10 = ExecuteCommandLists; device vtable slot 8 = CreateCommandQueue.
bool g_enable_exec_probe = true;
std::atomic<uint64_t> g_exec_total{0};        // cumulative command lists executed
std::atomic<uint64_t> g_exec_last_frame{0};
std::atomic<uint64_t> g_exec_frame_ctr{0};
std::atomic<bool>     g_queue_vtable_patched{false};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugExecTotal    = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugExecPerFrame = 0;

using PFN_ExecuteCommandLists =
    void (STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using PFN_CreateCommandQueue =
    HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**);
PFN_ExecuteCommandLists g_orig_ExecuteCommandLists = nullptr;
PFN_CreateCommandQueue  g_orig_CreateCommandQueue  = nullptr;
using PFN_CreateCommandList = HRESULT (STDMETHODCALLTYPE*)
    (ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*,
     ID3D12PipelineState*, REFIID, void**);
using PFN_ResourceBarrier = void (STDMETHODCALLTYPE*)
    (ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
using PFN_CopyResource = void (STDMETHODCALLTYPE*)
    (ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);
using PFN_OMSetRenderTargets = void (STDMETHODCALLTYPE*)
    (ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*,
     BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
// Slot 14. Used only to NAME the node behind a dispatch: the light-tile pass was identified in
// a capture by its group count, but a capture cannot say which frame-graph node issued it.
using PFN_Dispatch = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
// hk_Dispatch moved with the census; declared in Stereo/StereoInternal.hpp.

using PFN_ExecuteIndirect = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,
    ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64);
// hk_ExecuteIndirect moved with the census; declared in Stereo/StereoInternal.hpp.

// Direct draws, vtable slots 12 and 13. The dispatch census (slot 14) and the indirect census
// (slot 59) between them still miss ordinary geometry, which is what per-object effects like the
// scanner's object highlight are drawn with -- so "which nodes draw for MAIN and never for VRCAM"
// was unanswerable. Same shape as the other two censuses: aggregate per node, report the
// asymmetry, never bin by argument values (that produced a false lead once already).
using PFN_DrawInstanced = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,
    UINT, UINT, UINT, UINT);
using PFN_DrawIndexedInstanced = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,
    UINT, UINT, UINT, INT, UINT);
// hk_DrawInstanced moved with the census; declared in Stereo/StereoInternal.hpp.
// Slot 25. Needed only to know WHICH pso a draw runs under: the holographic sight's reticle is
// one specific pixel shader, and a shader can only be substituted at PSO-creation time, so the
// creation site has to be told which one to substitute. Identity travels as the PS bytecode
// hash, which is stable across runs; the pointer is not.
using PFN_SetPipelineState = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,
    ID3D12PipelineState*);
// hk_SetPipelineState moved with the census; declared in Stereo/StereoInternal.hpp.
// Slot 44. The sight quad's placement rides in the instance stream at slot 7; reading it at the
// draw is the only way to compare the weapon's ORIENTATION between the two views, which is now
// the one remaining input that can differ. (The collimated direction is eye-position independent
// by construction, so a per-eye disagreement can only come from per-view instance data.)
using PFN_IASetVertexBuffers = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,
    UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*);
// hk_IASetVertexBuffers moved with the census; declared in Stereo/StereoInternal.hpp.
// hk_DrawIndexedInstanced moved with the census; declared in Stereo/StereoInternal.hpp.

using PFN_CopyTextureRegion = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,
    const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT,
    const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);

using PFN_CopyBufferRegion = void (STDMETHODCALLTYPE*)
    (ID3D12GraphicsCommandList*, ID3D12Resource*, UINT64, ID3D12Resource*,
     UINT64, UINT64);
// vrcam post-DLSS crop fix: command-list viewport/scissor + reset hooks (slots 21/22/10).
using PFN_RSSetViewports = void (STDMETHODCALLTYPE*)
    (ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
using PFN_RSSetScissorRects = void (STDMETHODCALLTYPE*)
    (ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*);
using PFN_GfxReset = HRESULT (STDMETHODCALLTYPE*)
    (ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using CreateRTVFn = void (STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*,
    const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
PFN_CreateCommandList    g_orig_CreateCommandList = nullptr;
PFN_ResourceBarrier      g_orig_ResourceBarrier = nullptr;
PFN_CopyResource         g_orig_CopyResource = nullptr;
CreateRTVFn g_orig_CreateRTV = nullptr;
// Defined further down, with the constant-block probe it serves; declared here because the
// device vtable is patched long before that point.
using CreateCBVFn = void (STDMETHODCALLTYPE*)(ID3D12Device*,
    const D3D12_CONSTANT_BUFFER_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
CreateCBVFn g_orig_CreateCBV = nullptr;
// hk_CreateCBV is declared in Stereo/StereoInternal.hpp; its definition moved to another file.
// Exposure probe: defined with the rest of it further down, used by the barrier hook above it.
// expo_probe_copy moved with the census; declared in Stereo/StereoInternal.hpp.
// expo_mirror moved with the census; declared in Stereo/StereoInternal.hpp.
// expo_probe_report moved with the census; declared in Stereo/StereoInternal.hpp.
// tile_is_grid moved with the census; declared in Stereo/StereoInternal.hpp.
// cull_count_note moved with the census; declared in Stereo/StereoInternal.hpp.
// cull_count_report moved with the census; declared in Stereo/StereoInternal.hpp.
// tile_probe_copy moved with the census; declared in Stereo/StereoInternal.hpp.
// tile_probe_report moved with the census; declared in Stereo/StereoInternal.hpp.
static std::atomic<bool> g_rtv_hook_installed{false};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMirrorGameQueue = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMirrorReadyFence = 0;

std::mutex g_game_object_mtx;
ID3D12Device* g_game_device = nullptr;

// The D3D12 debug-layer message drain that lived here is DELETED, its question answered on
// 2026-08-29. Forced on from outside for this executable alone, the layer reports two errors
// continuously -- 538, a DiscardResource on a resource the runtime tracks as COMMON where
// UNORDERED_ACCESS is required, and 1314, a ClearUnorderedAccessViewUint whose descriptor is
// not from the bound heap. Both were measured over a minute of gameplay with the port's render
// hooks installed and again with them out entirely (bin/x64/vrport_nostereo.txt):
//
//     538    5.0/s with hooks   4.0/s without
//     1314  18.8/s with hooks  20.0/s without
//
// Identical. They are the engine's own, they do not crash a vanilla install, and the port
// neither causes nor multiplies them. Nothing here is a lead.
//
// Worth recording for whoever forces the layer on next: the layer costs a large part of the
// frame rate, and while it was on the 0x88 driver fault did not occur once in 1457 seconds
// against a median of about two minutes without it -- suggestive, and the reason the control
// run for that fault has to be done with the layer OFF.
ID3D12CommandQueue* g_game_queue = nullptr;
// Accessors for the game-view ImGui overlay (overlay_imgui.cpp).
extern "C" ID3D12Device*       CyberpunkVR_GetGameDevice() { return g_game_device; }
extern "C" ID3D12CommandQueue* CyberpunkVR_GetGameQueue()  { return g_game_queue; }

// Moved to src/Stereo/Mirror.cpp: the game-side copy objects, borrowed by the command-list hooks.

// hk_CreateRTV is declared in Stereo/StereoInternal.hpp; its definition moved to another file.
// hk_ResourceBarrier is declared in Stereo/StereoInternal.hpp; its definition moved to another file.
// hk_OMSetRenderTargets is declared in Stereo/StereoInternal.hpp; its definition moved to another file.
// hk_CopyBufferRegion moved with the census; declared in Stereo/StereoInternal.hpp.
// hk_RSSetViewports moved with the DLSS band to src/Stereo/Dlss.cpp; declared in
// Stereo/StereoInternal.hpp.
// hk_RSSetScissorRects moved with the DLSS band; declared in Stereo/StereoInternal.hpp.
// hk_GfxReset moved with the DLSS band; declared in Stereo/StereoInternal.hpp.
// POST-DLSS CROP FIX (D3D12-level, definitive): per Nsight, the crop is a vrcam-only post-DLSS
// fullscreen blit/tonemap (PipelineState_563) that READS the 2444 DLSS output and WRITES a 2444
// RT but with a render-res (1418) viewport+scissor -> only the top-left 1418 is filled = crop.
// MAIN has no such graphics pass (it composites via compute). The blit's viewport is NOT the
// view render-res field we can flip (VP+0x34 feeds pre-DLSS passes too), so we correct it at the
// D3D12 layer: on the SAME command-list/thread that records the vrcam DLSS eval, any viewport or
// scissor set to the vrcam RENDER size (1418) AFTER the eval is upscaled to the vrcam OUTPUT size
// (2444). Thread-local phase (set at vrcam eval, cleared at the command-list Reset that begins the
// next frame's recording) keeps this from touching pre-DLSS passes; the render-size match keeps it
// off MAIN (main never uses the square vrcam render size). Dims come from g_vrcam_dlss_r*/o*.
thread_local bool t_vrcam_dlss_post = false;   // (legacy, unused) vrcam post-DLSS phase marker
// THREAD-AGNOSTIC crop fix: OMSetRenderTargets and RSSetViewports/ScissorRects for a given draw
// run consecutively on the SAME command-list recording thread. So we capture the primary bound
// RT's dimensions per-thread here, and the viewport/scissor hooks upscale a viewport that
// under-fills that RT. This does not depend on which thread the DLSS eval ran on (the eval and
// the vrcam blit are NOT reliably co-threaded in the live frame graph).
thread_local UINT t_cur_rt_w = 0;   // primary bound RT width  on THIS thread
thread_local UINT t_cur_rt_h = 0;   // primary bound RT height on THIS thread
// Set while the vrcam CopyToTexture (sub_140377B58) node is recording on THIS thread: the crop
// pass whose viewport comes from a render-res SOURCE resource (not VP+0x34), so the VP-swap alone
// can't move it. While set, hk_RSSetViewports/Scissor upscale the render-res (1418) viewport to
// output (2444) -> the copy fills the whole 2444 target. Scoped to just this one node + vrcam.
thread_local bool t_copytotex = false;
// (removed: investigation-only viewport/blit diagnostics -- crop root FOUND = raster tonemap
//  sub_140768510 gated by group 20; fixed natively in Detour_RenderRes via view+0x17D0 match-main.
//  The stack-capture + band-aid are no longer needed, and the per-viewport RtlCaptureStackBackTrace
//  was pure overhead.)
// Broad RTV -> dims map (ANY format/size, unlike the RGBA8-only mirror candidate list) so the
// OMSetRenderTargets hook can tell the size of the bound RT for the R11G11B10 DLSS-output blit.
// `res` so a bound RTV can be resolved back to its texture -- needed to name the surface
// RenderVisionElements draws the scanner's object outline into (see the vision snapshot).
// EVERY RTV THE GAME CREATES, BY DESCRIPTOR HANDLE. Read on the recording threads to answer "what
// is this bind pointing at", which is how the HUD surface is identified and how the vrcam output
// is recognised -- so a lookup that comes back empty does not degrade anything, it switches a
// feature off.
//
// It used to be 2048 entries with `if (n >= size) return;` -- silently stop accepting, forever.
// Past that point every newly created descriptor was invisible, and a frame-graph rebuild creates
// a whole new set: the HUD node kept binding its surface and we could no longer say what the
// handle meant, so `[hud] surface named by DrawHUD` stopped appearing and the second eye lost the
// HUD for the session. The same saturation is why a [rtvpick] miss reported "descriptor never seen
// created" for a target the engine had plainly just created.
//
// The metadata cache now lives in Capture.cpp. Active bindings refresh its LRU
// position, and swapchain buffers are pinned independently of descriptor age.
// Resource/dimension reads are one locked snapshot; an atomic handle alone did
// not protect the plain fields from a concurrent descriptor replacement.
std::atomic<uint32_t> g_rtv_dim_count{0};
std::atomic<bool> g_rtv_dim_wrapped_logged{false};
// How often the HUD node binds a target, and how often we cannot say what that bind points at.
// The second number rising with the first is the map above failing to answer, which is the whole
// difference between "the HUD moved" and "we went blind to it".
std::atomic<uint64_t> g_hud_node_binds{0};
std::atomic<uint64_t> g_hud_node_unresolved{0};
// d12_present_thread and d12_submit_mirror_copy moved to src/Stereo/Mirror.cpp; both are declared
// in Stereo/StereoInternal.hpp. A `static` forward declaration here would promise a definition in
// THIS file -- the shape that has now appeared in six extractions.

static void STDMETHODCALLTYPE Hook_ExecuteCommandLists(
        ID3D12CommandQueue* self, UINT n, ID3D12CommandList* const* lists) {
    CVR_DIAGNOSTIC(CyberpunkVR_DebugExecTotal = g_exec_total.fetch_add(n, std::memory_order_relaxed) + n);
    // Do the single 11on12 copy+present only once the game actually SUBMITS the
    // command list that wrote the vrcam dtex (the one that recorded the blit's
    // RENDER_TARGET->read barrier). Queue ordering then guarantees our copy reads
    // the freshly written frame. Non-blocking (Flush + Present(0)) => no FPS drop.
    ID3D12GraphicsCommandList* pending =
        g_mirror_pending_list.load(std::memory_order_acquire);
    bool submits_blit = false;
    if (pending && lists) {
        for (UINT i = 0; i < n; ++i) {
            if (lists[i] == reinterpret_cast<ID3D12CommandList*>(pending)) {
                submits_blit = true; break;
            }
        }
    }
    auto srSubmission=cvr::framegen::SrBeforeSubmit(self,n,lists);
    {
        std::lock_guard submissionLock(cvr::camera::ImageSubmissionMutex());
        if(self==g_game_queue)cvr::framegen::BeforeGameCommands(self);
        auto resources = cvr::gpu::PrepareCommandResources(n, lists);
        auto imageSubmission=cvr::camera::PrepareImagePoseLists(self,n,lists);
        auto previewImageSubmission=cvr::menu_preview::PreparePreviewImageLists(self,n,lists);
        auto previewPixels=cvr::menu_preview::PreparePreviewPixels(n,lists,self);
        auto uiSubmission=cvr::menu_preview::PrepareUiSubmission(n,lists);
        auto editorUiSubmission=cvr::menu_preview::PrepareEditorUiSubmission(self,n,lists);
        auto nativeColorSubmission=cvr::render::PrepareNativeColor(self,n,lists);
        auto inputSubmission=cvr::framegen::PrepareInputSubmission(n,lists);
        cvr::stereo::gpu_probe::BeforeSubmit(self,n,lists);
        ColorPathAuditBeforeSubmit(self,n,lists);
        cvr::framegen::NativeFgReadbackBeforeSubmit(self,n,lists);
        cvr::framegen::NativeFgOutputBeforeSubmit(self,n,lists);
        g_orig_ExecuteCommandLists(self,n,lists);
        const auto actualUiSubmission=cvr::menu_preview::UiSubmitted(self,std::move(uiSubmission));
        cvr::menu_preview::EditorUiSubmitted(self,std::move(editorUiSubmission),g_orig_ExecuteCommandLists);
        cvr::menu_preview::SubmittedPreviewPixels(self,std::move(previewPixels),g_orig_ExecuteCommandLists,actualUiSubmission);
        cvr::framegen::NativeFgReadbackSubmitted(self);
        cvr::framegen::NativeFgOutputSubmitted(self);
        ColorPathAuditSubmitted(self);
        cvr::camera::CommitImagePoseLists(self,std::move(imageSubmission));
        cvr::menu_preview::CommitPreviewImageLists(self,std::move(previewImageSubmission));
        cvr::render::CommitNativeColor(self,std::move(nativeColorSubmission));
        cvr::gpu::SubmitCommandResources(self, std::move(resources));
        cvr::stereo::gpu_probe::Submitted(self,n,lists);
        cvr::framegen::Submitted(inputSubmission,self);
    }
    cvr::framegen::SrSubmitted(srSubmission,self,n,lists);
    // The game just submitted the list that wrote the vrcam final. That target rests
    // permanently in RENDER_TARGET (never read back -> no RT->read barrier exists),
    // so we copy it out ourselves with one tiny submit on the game queue right here,
    // from its known fixed state. Queue order guarantees we read the fresh frame.
    if (submits_blit && self == g_game_queue) {
        g_mirror_pending_list.store(nullptr, std::memory_order_release);
        d12_submit_mirror_copy(self);
    }
}

static void patch_queue_vtable(void* queue) {
    if (!queue) return;
    bool e = false;
    if (!g_queue_vtable_patched.compare_exchange_strong(e, true)) return;
    void** vt = *reinterpret_cast<void***>(queue);
    DWORD oldp = 0;
    if (VirtualProtect(&vt[10], sizeof(void*), PAGE_READWRITE, &oldp)) {
        g_orig_ExecuteCommandLists = reinterpret_cast<PFN_ExecuteCommandLists>(vt[10]);
        vt[10] = reinterpret_cast<void*>(&Hook_ExecuteCommandLists);
        DWORD junk = 0;
        VirtualProtect(&vt[10], sizeof(void*), oldp, &junk);
        log("[exec] ExecuteCommandLists hooked (queue=%p orig=%p)",
            queue, (void*)g_orig_ExecuteCommandLists);
    } else {
        g_queue_vtable_patched.store(false, std::memory_order_release);
    }
}

static HRESULT STDMETHODCALLTYPE Hook_CreateCommandQueue(
        ID3D12Device* self, const D3D12_COMMAND_QUEUE_DESC* desc, REFIID riid, void** out) {
    HRESULT hr = g_orig_CreateCommandQueue(self, desc, riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        auto* queue = reinterpret_cast<ID3D12CommandQueue*>(*out);
        if(!cvr::gpu::RegisterNativeObject(queue))log("[exec] native queue identity registration failed queue=%p",queue);
        if (desc && desc->Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            std::lock_guard<std::mutex> lock(g_game_object_mtx);
            if (!g_game_queue) {
                queue->AddRef();
                g_game_queue = queue;
                CyberpunkVR_DebugMirrorGameQueue = reinterpret_cast<uint64_t>(queue);
                log("[mirror] captured game DIRECT queue=%p device=%p", queue, self);
            }
        }
        patch_queue_vtable(queue);
    }
    return hr;
}

// Filled by the PSO-creation hooks below; consumed by the sight probe further down.
// pso_ids_record moved with the census; declared in Stereo/StereoInternal.hpp.
// hud_adopt_by_node moved with the HUD; declared in Stereo/StereoInternal.hpp.
std::atomic<uint64_t> g_hud_snap_tick{0};
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_HudNodeRva;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_HudNodeRva2;
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_HudByNode;
// fnv1a moved with the census; declared in Stereo/StereoInternal.hpp.
// sight_ps_dump moved with the census; declared in Stereo/StereoInternal.hpp.
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_SightPsHash;
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_SightPsDump;

static std::array<CommandListVtableHook, 16> g_command_list_vtable_hooks{};
static std::atomic<uint32_t> g_command_list_vtable_hook_count{0};
static std::mutex g_command_list_vtable_hook_mtx;

const CommandListVtableHook* command_list_hook_entry(
        ID3D12GraphicsCommandList* command_list) {
    if (!command_list) return nullptr;
    void** vtable = *reinterpret_cast<void***>(command_list);
    const uint32_t count =
        g_command_list_vtable_hook_count.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < count; ++i) {
        if (g_command_list_vtable_hooks[i].vtable == vtable)
            return &g_command_list_vtable_hooks[i];
    }
    return nullptr;
}
PFN_OMSetRenderTargets command_list_original_om(
        ID3D12GraphicsCommandList* command_list) {
    const CommandListVtableHook* e = command_list_hook_entry(command_list);
    return e ? e->original : nullptr;
}

static void STDMETHODCALLTYPE ProbeHeaps(ID3D12GraphicsCommandList* list,UINT count,ID3D12DescriptorHeap* const* heaps) {
    const auto* e=command_list_hook_entry(list);if(!e || !e->heaps_original)return;
    e->heaps_original(list,count,heaps);cvr::stereo::scene_state::Heaps(list,count,heaps);
    cvr::menu_preview::UiDescriptorHeaps(list,count,heaps);
}
static void STDMETHODCALLTYPE ProbeTopology(ID3D12GraphicsCommandList* list,D3D12_PRIMITIVE_TOPOLOGY topology) {
    const auto* e=command_list_hook_entry(list);if(!e || !e->topology_original)return;
    e->topology_original(list,topology);cvr::stereo::scene_state::Topology(list,topology);
}
static void STDMETHODCALLTYPE ProbeStencil(ID3D12GraphicsCommandList* list,UINT stencil) {
    const auto* e=command_list_hook_entry(list);if(!e || !e->stencil_original)return;
    e->stencil_original(list,stencil);cvr::stereo::scene_state::Stencil(list,stencil);
}
static void STDMETHODCALLTYPE ProbeGraphicsRoot(ID3D12GraphicsCommandList* list,ID3D12RootSignature* root) {
    const auto* entry=command_list_hook_entry(list);if(!entry || !entry->groot_original)return;
    entry->groot_original(list,root);cvr::stereo::native_probe::RootSignature(list,root);
    cvr::menu_preview::UiGraphicsRootSignature(list,root);
}
static void STDMETHODCALLTYPE ProbeGraphicsTable(ID3D12GraphicsCommandList* list,UINT index,D3D12_GPU_DESCRIPTOR_HANDLE table) {
    const auto* entry=command_list_hook_entry(list);if(!entry || !entry->grootdt_original)return;
    const auto nativeCaller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    entry->grootdt_original(list,index,table);cvr::stereo::native_probe::RootTable(list,index,table);
    cvr::menu_preview::UiGraphicsRootTable(list,index,table,nativeCaller);
}
static void STDMETHODCALLTYPE ProbeGraphicsCbv(ID3D12GraphicsCommandList* list,UINT index,D3D12_GPU_VIRTUAL_ADDRESS address) {
    const auto* entry=command_list_hook_entry(list);if(!entry || !entry->grootcbv_original)return;
    entry->grootcbv_original(list,index,address);cvr::stereo::native_probe::RootCbv(list,index,address);
    if(cvr::menu_preview::UiRasterDiagnosticsEnabled())cvr::menu_preview::UiGraphicsConstantBuffer(list,index,address);
}
static void STDMETHODCALLTYPE ProbeIndexBuffer(ID3D12GraphicsCommandList* list,const D3D12_INDEX_BUFFER_VIEW* view) {
    const auto* entry=command_list_hook_entry(list);if(!entry || !entry->iaib_original)return;
    entry->iaib_original(list,view);cvr::stereo::native_probe::IndexBuffer(list,view);
}
static void STDMETHODCALLTYPE ProbePredication(ID3D12GraphicsCommandList* list,ID3D12Resource* buffer,UINT64 offset,D3D12_PREDICATION_OP operation) {
    const auto* entry=command_list_hook_entry(list);if(!entry || !entry->predication_original)return;
    entry->predication_original(list,buffer,offset,operation);cvr::stereo::native_probe::Predication(list,buffer);
}
static void STDMETHODCALLTYPE ProbeShadingRate(ID3D12GraphicsCommandList5* list,D3D12_SHADING_RATE rate,const D3D12_SHADING_RATE_COMBINER* combiners) {
    const auto* e=command_list_hook_entry(list);if(!e || !e->shading_rate_original)return;
    e->shading_rate_original(list,rate,combiners);cvr::stereo::scene_state::ShadingRate(list,rate,combiners);
}
static void STDMETHODCALLTYPE ProbeShadingImage(ID3D12GraphicsCommandList5* list,ID3D12Resource* image) {
    const auto* e=command_list_hook_entry(list);if(!e || !e->shading_image_original)return;
    e->shading_image_original(list,image);cvr::stereo::scene_state::ShadingRateImage(list,image);
}
static HRESULT STDMETHODCALLTYPE PreviewClose(ID3D12GraphicsCommandList* self){
    const auto* e=command_list_hook_entry(self);if(!e||!e->close_original)return E_FAIL;
    if(cvr::menu_preview::EditorUiPrivateCommands())return e->close_original(self);
    const bool locked=cvr::menu_preview::LockPreviewPixelLifecycle();
    const auto result=e->close_original(self);
    cvr::menu_preview::ClosePreviewPixelList(self,SUCCEEDED(result));
    cvr::menu_preview::UiCloseList(self,SUCCEEDED(result));
    cvr::menu_preview::CloseEditorUiList(self,SUCCEEDED(result));
    cvr::menu_preview::UnlockPreviewPixelLifecycle(locked);return result;
}
static void STDMETHODCALLTYPE ProbePipelineState(ID3D12GraphicsCommandList* list,ID3D12PipelineState* pipeline){
    hk_SetPipelineState(list,pipeline);
    if(!cvr::stereo::probe::internalCommands)cvr::menu_preview::UiPipelineState(list,pipeline);
}
static void patch_command_list_vtable(void* command_list) {
    if (!command_list) return;
    void** vtable = *reinterpret_cast<void***>(command_list);
    std::lock_guard<std::mutex> lock(g_command_list_vtable_hook_mtx);
    const uint32_t count = g_command_list_vtable_hook_count.load(std::memory_order_relaxed);
    for (uint32_t i=0;i<count;++i) if(g_command_list_vtable_hooks[i].vtable==vtable)return;
    if(count>=g_command_list_vtable_hooks.size())return;
    const bool probeEnabled=cvr::stereo::native_probe::Enabled();
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList5> list5;
    const auto list5Result=probeEnabled?static_cast<ID3D12GraphicsCommandList*>(command_list)->QueryInterface(IID_PPV_ARGS(&list5)):E_NOINTERFACE;
    const bool sameList5=SUCCEEDED(list5Result) && list5.Get()==command_list && *reinterpret_cast<void***>(list5.Get())==vtable;

    // Other render workers already use this shared vtable. Publish every
    // original BEFORE exposing even the first detour. The old per-slot patch
    // published the record last; a concurrent detour then returned without
    // recording its native command (including barriers, root tables and draws).
    const CommandListVtableHook entry{
        vtable,
        reinterpret_cast<PFN_OMSetRenderTargets>(vtable[46]),
        reinterpret_cast<PFN_ResourceBarrier>(vtable[26]),
        reinterpret_cast<PFN_ResourceBarrier>(vtable[26]),
        reinterpret_cast<PFN_CopyResource>(vtable[17]),
        reinterpret_cast<PFN_CopyTextureRegion>(vtable[16]),
        reinterpret_cast<PFN_ExecuteIndirect>(vtable[59]),
        reinterpret_cast<PFN_DrawInstanced>(vtable[12]),
        reinterpret_cast<PFN_DrawIndexedInstanced>(vtable[13]),
        reinterpret_cast<PFN_CopyBufferRegion>(vtable[15]),
        reinterpret_cast<PFN_Dispatch>(vtable[14]),
        reinterpret_cast<PFN_RSSetViewports>(vtable[21]),
        reinterpret_cast<PFN_RSSetScissorRects>(vtable[22]),
        reinterpret_cast<PFN_GfxReset>(vtable[10]),
        reinterpret_cast<PFN_SetPipelineState>(vtable[25]),
        reinterpret_cast<PFN_IASetVertexBuffers>(vtable[44]),
        reinterpret_cast<PFN_ClearDepthStencilView>(vtable[47]),
        reinterpret_cast<PFN_SetComputeRootSignature>(vtable[29]),
        reinterpret_cast<PFN_SetComputeRootDescriptorTable>(vtable[31]),
        reinterpret_cast<PFN_SetComputeRootSignature>(vtable[30]),
        reinterpret_cast<PFN_SetComputeRootDescriptorTable>(vtable[32]),
        reinterpret_cast<PFN_SetGraphicsRootCbv>(vtable[38]),
        reinterpret_cast<PFN_IASetIndexBuffer>(vtable[43]),
        reinterpret_cast<PFN_SetPredication>(vtable[55]),
        reinterpret_cast<PFN_ProbeHeaps>(vtable[28]),reinterpret_cast<PFN_ProbeTopology>(vtable[20]),reinterpret_cast<PFN_ProbeStencil>(vtable[24]),
        sameList5?reinterpret_cast<PFN_ProbeShadingRate>(vtable[77]):nullptr,
        sameList5?reinterpret_cast<PFN_ProbeShadingImage>(vtable[78]):nullptr,
        probeEnabled && (sameList5 || list5Result==E_NOINTERFACE),
        reinterpret_cast<HRESULT (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*)>(vtable[9])};
    struct Slot {size_t index;void* detour;};
    const Slot slots[]{
        {9,reinterpret_cast<void*>(&PreviewClose)},
        {46,reinterpret_cast<void*>(&hk_OMSetRenderTargets)},
        {26,reinterpret_cast<void*>(&hk_ResourceBarrier)},
        {15,reinterpret_cast<void*>(&hk_CopyBufferRegion)},
        {59,reinterpret_cast<void*>(&hk_ExecuteIndirect)},
        {14,reinterpret_cast<void*>(&hk_Dispatch)},
        {21,reinterpret_cast<void*>(&hk_RSSetViewports)},
        {22,reinterpret_cast<void*>(&hk_RSSetScissorRects)},
        {10,reinterpret_cast<void*>(&hk_GfxReset)},
        {12,reinterpret_cast<void*>(&hk_DrawInstanced)},
        {13,reinterpret_cast<void*>(&hk_DrawIndexedInstanced)},
        {44,reinterpret_cast<void*>(&hk_IASetVertexBuffers)},
        {29,reinterpret_cast<void*>(&hk_SetComputeRootSignature)},
        {31,reinterpret_cast<void*>(&hk_SetComputeRootDescriptorTable)},
        {25,reinterpret_cast<void*>(&ProbePipelineState)},
        {47,reinterpret_cast<void*>(&hk_ClearDepthStencilView)}};
    const Slot probeSlots[]{{30,reinterpret_cast<void*>(&ProbeGraphicsRoot)},
        {32,reinterpret_cast<void*>(&ProbeGraphicsTable)},{38,reinterpret_cast<void*>(&ProbeGraphicsCbv)},
        {43,reinterpret_cast<void*>(&ProbeIndexBuffer)},{17,reinterpret_cast<void*>(&probe_CopyResource)},
        {16,reinterpret_cast<void*>(&probe_CopyTextureRegion)},
        {55,reinterpret_cast<void*>(&ProbePredication)},{28,reinterpret_cast<void*>(&ProbeHeaps)},
        {20,reinterpret_cast<void*>(&ProbeTopology)},{24,reinterpret_cast<void*>(&ProbeStencil)}};
    // Install lifetime/copy hooks before a menu exists. Per-frame observation
    // remains gated by a fresh preview inside the callbacks themselves.
    const size_t firstSlot=cvr::menu_preview::PreviewMetadataRequested()?9:10;const size_t lastSlot=sameList5?78:59;
    const size_t bytes=(lastSlot-firstSlot+1)*sizeof(void*);
    DWORD oldProtection{};
    if(!VirtualProtect(vtable+firstSlot,bytes,PAGE_READWRITE,&oldProtection)) {
        log("[mirror] cannot make command-list vtable writable: %p",vtable);return;
    }
    g_command_list_vtable_hooks[count]=entry;
    g_command_list_vtable_hook_count.store(count+1,std::memory_order_release);
    for(const auto& slot:slots)if(slot.index!=9||cvr::menu_preview::PreviewMetadataRequested())
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(vtable+slot.index),slot.detour);
    for(const auto& slot:probeSlots)if(probeEnabled || (cvr::menu_preview::UiStereoRequested()&&(slot.index==28||slot.index==30||slot.index==32)))
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(vtable+slot.index),slot.detour);
    if(sameList5){
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(vtable+77),reinterpret_cast<void*>(&ProbeShadingRate));
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(vtable+78),reinterpret_cast<void*>(&ProbeShadingImage));
    }
    DWORD ignored{};
    VirtualProtect(vtable+firstSlot,bytes,oldProtection,&ignored);
    log("[mirror] command-list hooked list=%p vt=%p om=%p rb=%p cr=%p",
        command_list,vtable,(void*)entry.original,(void*)entry.barrier_original,(void*)entry.copyres);
}

// ID3D12Device slots 27 and 29. Only to answer "which resource owns this GPU address" -- a
// vertex-buffer view carries an address and nothing else, and D3D12 offers no way back.
using PFN_CreateCommittedResource = HRESULT (STDMETHODCALLTYPE*)(
    ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using PFN_CreatePlacedResource = HRESULT (STDMETHODCALLTYPE*)(
    ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*,
    D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
static PFN_CreateCommittedResource g_orig_CreateCommitted = nullptr;
static PFN_CreatePlacedResource    g_orig_CreatePlaced = nullptr;
using PFN_CreateCommitted1=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device4*,const D3D12_HEAP_PROPERTIES*,D3D12_HEAP_FLAGS,const D3D12_RESOURCE_DESC*,D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,ID3D12ProtectedResourceSession*,REFIID,void**);
using PFN_CreateCommitted2=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device8*,const D3D12_HEAP_PROPERTIES*,D3D12_HEAP_FLAGS,const D3D12_RESOURCE_DESC1*,D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,ID3D12ProtectedResourceSession*,REFIID,void**);
using PFN_CreateCommitted3=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device10*,const D3D12_HEAP_PROPERTIES*,D3D12_HEAP_FLAGS,const D3D12_RESOURCE_DESC1*,D3D12_BARRIER_LAYOUT,const D3D12_CLEAR_VALUE*,ID3D12ProtectedResourceSession*,UINT32,const DXGI_FORMAT*,REFIID,void**);
static PFN_CreateCommitted1 g_orig_CreateCommitted1{};
static PFN_CreateCommitted2 g_orig_CreateCommitted2{};
static PFN_CreateCommitted3 g_orig_CreateCommitted3{};
static void StampCommittedAllocation(HRESULT hr,void** out,unsigned api){
    if(FAILED(hr)||!out||!*out)return;
    auto* resource=static_cast<ID3D12Resource*>(*out);
    const auto marked=cvr::gpu::MarkResourceAllocation(resource,true);
    static std::atomic<uint32_t> rows{};
    const auto desc=resource->GetDesc();
    if(desc.Width==1920&&desc.Height==1920&&rows.fetch_add(1)<12)
        log("[allocation] committed-api=%u resource=%p mark=%08X size=%llux%u",api,resource,unsigned(marked),desc.Width,desc.Height);
}
static HRESULT STDMETHODCALLTYPE Hook_CreateCommitted1(ID3D12Device4* self,const D3D12_HEAP_PROPERTIES* hp,D3D12_HEAP_FLAGS flags,const D3D12_RESOURCE_DESC* desc,D3D12_RESOURCE_STATES state,const D3D12_CLEAR_VALUE* clear,ID3D12ProtectedResourceSession* protectedSession,REFIID iid,void** out){
    const auto hr=g_orig_CreateCommitted1(self,hp,flags,desc,state,clear,protectedSession,iid,out);StampCommittedAllocation(hr,out,1);return hr;
}
static HRESULT STDMETHODCALLTYPE Hook_CreateCommitted2(ID3D12Device8* self,const D3D12_HEAP_PROPERTIES* hp,D3D12_HEAP_FLAGS flags,const D3D12_RESOURCE_DESC1* desc,D3D12_RESOURCE_STATES state,const D3D12_CLEAR_VALUE* clear,ID3D12ProtectedResourceSession* protectedSession,REFIID iid,void** out){
    const auto hr=g_orig_CreateCommitted2(self,hp,flags,desc,state,clear,protectedSession,iid,out);StampCommittedAllocation(hr,out,2);return hr;
}
static HRESULT STDMETHODCALLTYPE Hook_CreateCommitted3(ID3D12Device10* self,const D3D12_HEAP_PROPERTIES* hp,D3D12_HEAP_FLAGS flags,const D3D12_RESOURCE_DESC1* desc,D3D12_BARRIER_LAYOUT layout,const D3D12_CLEAR_VALUE* clear,ID3D12ProtectedResourceSession* protectedSession,UINT32 formatCount,const DXGI_FORMAT* formats,REFIID iid,void** out){
    const auto hr=g_orig_CreateCommitted3(self,hp,flags,desc,layout,clear,protectedSession,formatCount,formats,iid,out);StampCommittedAllocation(hr,out,3);return hr;
}
template<class Device,class Fn> static void HookCommittedAllocationMethod(ID3D12Device* base,UINT slot,Fn& original,Fn hook){
    Microsoft::WRL::ComPtr<Device> device;
    if(FAILED(base->QueryInterface(IID_PPV_ARGS(&device)))||!device)return;
    auto** table=*reinterpret_cast<void***>(device.Get());
    DWORD protection{};if(!VirtualProtect(table+slot,sizeof(void*),PAGE_READWRITE,&protection)){log("[allocation] hook failed slot=%u",slot);return;}
    original=reinterpret_cast<Fn>(table[slot]);
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(table+slot),reinterpret_cast<void*>(hook));
    DWORD ignored{};VirtualProtect(table+slot,sizeof(void*),protection,&ignored);
    log("[allocation] versioned committed resource hook slot=%u device=%p",slot,device.Get());
}
// buf_note moved with the census; declared in Stereo/StereoInternal.hpp.

static void buf_note_created(void* out, const D3D12_RESOURCE_DESC* d) {
    if (!out || !d || d->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return;
    if (d->Width < 4096) return;                    // instance/vertex streams, not tiny scratch
    auto* res = static_cast<ID3D12Resource*>(out);
    uint64_t va = 0;
    __try { va = res->GetGPUVirtualAddress(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    buf_note(res, va, d->Width);
}

static HRESULT STDMETHODCALLTYPE Hook_CreateCommittedResource(
        ID3D12Device* self, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
        const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES st,
        const D3D12_CLEAR_VALUE* cv, REFIID riid, void** out) {
    HRESULT hr = g_orig_CreateCommitted(self, hp, hf, d, st, cv, riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        if(cvr::framegen::NativeFgStereoRequested()||cvr::menu_preview::UiConsumerEnabled())cvr::gpu::MarkResourceAllocation(static_cast<ID3D12Resource*>(*out),true);
        NameGpuObjectProvenance(static_cast<IUnknown*>(*out),L"Committed",_ReturnAddress());
        buf_note_created(*out, d);
        if (d) cvr::diagnostics::ObserveResourceCreation(static_cast<ID3D12Resource*>(*out), *d);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreatePlacedResource(
        ID3D12Device* self, ID3D12Heap* heap, UINT64 off, const D3D12_RESOURCE_DESC* d,
        D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID riid, void** out) {
    HRESULT hr = g_orig_CreatePlaced(self, heap, off, d, st, cv, riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        if(cvr::framegen::NativeFgStereoRequested()||cvr::menu_preview::UiConsumerEnabled())cvr::gpu::MarkPlacedResourceAllocation(self,static_cast<ID3D12Resource*>(*out),heap,off,d);
        NameGpuObjectProvenance(static_cast<IUnknown*>(*out),L"Placed",_ReturnAddress());
        buf_note_created(*out, d);
        if (d) cvr::diagnostics::ObserveResourceCreation(static_cast<ID3D12Resource*>(*out), *d);
    }
    return hr;
}

// ID3D12Device slot 16. There is no reflection from an ID3D12RootSignature back to its
// description, so the only way to learn the binding contract our replacement shaders must live
// inside is to keep the blob at creation. What we are looking for is a root parameter this
// material does NOT use -- a spare root CBV or a set of root constants -- because occupying one
// of those needs no signature change at all: the PSO keeps the game's signature, nothing is
// rebound, and the muzzle direction can simply be set before the draw.
using PFN_CreateRootSignature = HRESULT (STDMETHODCALLTYPE*)(
    ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
static PFN_CreateRootSignature g_orig_CreateRootSig = nullptr;
static std::unordered_map<void*, std::vector<uint8_t>> g_rootsig_blobs;
static std::mutex g_rootsig_mtx;

static HRESULT STDMETHODCALLTYPE Hook_CreateRootSignature(
        ID3D12Device* self, UINT nodeMask, const void* blob, SIZE_T len,
        REFIID riid, void** out) {
    HRESULT hr = g_orig_CreateRootSig(self, nodeMask, blob, len, riid, out);
    if (SUCCEEDED(hr) && out && *out && blob && len && len < (1u << 20)) {
        cvr::stereo::native_probe::RootCreated(static_cast<ID3D12RootSignature*>(*out),blob,len);
        std::lock_guard<std::mutex> lk(g_rootsig_mtx);
        if (g_rootsig_blobs.size() < 4096)
            g_rootsig_blobs[*out].assign(static_cast<const uint8_t*>(blob),
                                         static_cast<const uint8_t*>(blob) + len);
    }
    return hr;
}

// Print the layout once, for the signature the sight's pipeline uses.
static void rootsig_dump(void* rs) {
    if (!rs) return;
    static std::atomic<void*> s_done{nullptr};
    void* expected = nullptr;
    if (!s_done.compare_exchange_strong(expected, rs)) return;
    std::vector<uint8_t> blob;
    {
        std::lock_guard<std::mutex> lk(g_rootsig_mtx);
        auto it = g_rootsig_blobs.find(rs);
        if (it == g_rootsig_blobs.end()) {
            log("[rootsig] sight signature %p: blob not captured (created before the hook)", rs);
            return;
        }
        blob = it->second;
    }
    ID3D12VersionedRootSignatureDeserializer* des = nullptr;
    if (FAILED(D3D12CreateVersionedRootSignatureDeserializer(
            blob.data(), blob.size(), IID_PPV_ARGS(&des))) || !des) {
        log("[rootsig] sight signature %p: deserialize failed (%zu bytes)", rs, blob.size());
        return;
    }
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* v = nullptr;
    if (FAILED(des->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &v)) || !v) {
        des->Release();
        log("[rootsig] sight signature %p: no 1.1 view", rs);
        return;
    }
    const auto& d = v->Desc_1_1;
    log("[rootsig] sight signature %p: %u params, %u samplers, flags=0x%X",
        rs, d.NumParameters, d.NumStaticSamplers, (unsigned)d.Flags);
    for (UINT i = 0; i < d.NumParameters; ++i) {
        const auto& p = d.pParameters[i];
        const unsigned vis = (unsigned)p.ShaderVisibility;
        switch (p.ParameterType) {
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            log("[rootsig]  [%2u] CONSTANTS b%u space%u x%u  vis=%u", i,
                p.Constants.ShaderRegister, p.Constants.RegisterSpace,
                p.Constants.Num32BitValues, vis);
            break;
        case D3D12_ROOT_PARAMETER_TYPE_CBV:
            log("[rootsig]  [%2u] CBV       b%u space%u  vis=%u", i,
                p.Descriptor.ShaderRegister, p.Descriptor.RegisterSpace, vis);
            break;
        case D3D12_ROOT_PARAMETER_TYPE_SRV:
            log("[rootsig]  [%2u] SRV       t%u space%u  vis=%u", i,
                p.Descriptor.ShaderRegister, p.Descriptor.RegisterSpace, vis);
            break;
        case D3D12_ROOT_PARAMETER_TYPE_UAV:
            log("[rootsig]  [%2u] UAV       u%u space%u  vis=%u", i,
                p.Descriptor.ShaderRegister, p.Descriptor.RegisterSpace, vis);
            break;
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
            char r[400]; int u = 0; r[0] = 0;
            for (UINT k = 0; k < p.DescriptorTable.NumDescriptorRanges && u < 340; ++k) {
                const auto& rг = p.DescriptorTable.pDescriptorRanges[k];
                const char* t = rг.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_CBV ? "b"
                              : rг.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SRV ? "t"
                              : rг.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV ? "u" : "s";
                u += snprintf(r + u, sizeof(r) - u, "%s%u+%u/sp%u ", t,
                              rг.BaseShaderRegister, rг.NumDescriptors, rг.RegisterSpace);
            }
            log("[rootsig]  [%2u] TABLE     vis=%u  %s", i, vis, r);
            break;
        }
        default: log("[rootsig]  [%2u] type=%u", i, (unsigned)p.ParameterType); break;
        }
    }
    des->Release();
}

// ID3D12Device slot 10 and ID3D12Device2 slot 47. Both are entry points the engine may use, and
// which one it takes is not worth guessing -- hooking both costs one pointer each. All they do
// here is remember (pso -> shader hashes); substitution, when it comes, happens in the same place.
using PFN_CreateGraphicsPipelineState = HRESULT (STDMETHODCALLTYPE*)(
    ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using PFN_CreatePipelineState = HRESULT (STDMETHODCALLTYPE*)(
    ID3D12Device*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
static PFN_CreateGraphicsPipelineState g_orig_CreateGfxPso = nullptr;
static PFN_CreatePipelineState         g_orig_CreatePso = nullptr;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPsoGfx = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPsoStream = 0;

// The replacement is loaded from a FILE rather than baked into the DLL. Iterating on it then
// costs a dxc run and a game restart instead of a full rebuild, and the shader stays readable
// next to the binary it patches.
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_SightPsSwap = 1;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugSightSwaps = 0;
// Measured: this pixel shader is used by TWO pipelines and only one of them draws the reticle.
// Keying the swap on the PAIR (pixel AND vertex hash) picks that one exactly, instead of relying
// on the other pipeline failing to build.
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_SightVsHash = 0x9228439BF72D91DBull;
static std::vector<uint8_t> g_sight_blob;      // pixel
static std::vector<uint8_t> g_sight_vs_blob;   // vertex
static std::atomic<int> g_sight_blob_state{0};   // 0 untried, 1 loaded, -1 missing

static bool sight_blob_ready() {
    int st = g_sight_blob_state.load(std::memory_order_acquire);
    if (st) return st > 0;
    static std::mutex mtx;
    std::lock_guard<std::mutex> lk(mtx);
    st = g_sight_blob_state.load(std::memory_order_relaxed);
    if (st) return st > 0;
    char path[MAX_PATH]{};
    HMODULE self = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&sight_blob_ready), &self) && self &&
        GetModuleFileNameA(self, path, MAX_PATH)) {
        char* slash = strrchr(path, '\\');
        if (slash) {
            *(slash + 1) = 0;
            const size_t dirLen = strlen(path);
            bool bothOk = true;
            for (int which = 0; which < 2 && bothOk; ++which) {
                path[dirLen] = 0;
                strcat_s(path, which ? "CyberpunkVR_SightVs.dxil" : "CyberpunkVR_SightPs.dxil");
                std::vector<uint8_t>& dstBlob = which ? g_sight_vs_blob : g_sight_blob;
                dstBlob.clear();
                HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (f != INVALID_HANDLE_VALUE) {
                    LARGE_INTEGER sz{};
                    if (GetFileSizeEx(f, &sz) && sz.QuadPart > 64 && sz.QuadPart < (4 << 20)) {
                        dstBlob.resize(static_cast<size_t>(sz.QuadPart));
                        DWORD got = 0;
                        if (!(ReadFile(f, dstBlob.data(), static_cast<DWORD>(sz.QuadPart),
                                       &got, nullptr) && got == sz.QuadPart &&
                              memcmp(dstBlob.data(), "DXBC", 4) == 0)) {
                            dstBlob.clear();
                        }
                    }
                    CloseHandle(f);
                }
                if (dstBlob.empty()) {
                    log("[pso] sight replacement NOT found (%s) -- original shaders kept", path);
                    bothOk = false;
                }
            }
            // Both or neither: the pixel shader reads the glass size the vertex shader writes,
            // so half a swap would be worse than none.
            if (bothOk) {
                g_sight_blob_state.store(1, std::memory_order_release);
                log("[pso] sight replacement loaded: PS %zu B, VS %zu B",
                    g_sight_blob.size(), g_sight_vs_blob.size());
                return true;
            }
            g_sight_blob.clear();
            g_sight_vs_blob.clear();
        }
    }
    g_sight_blob_state.store(-1, std::memory_order_release);
    return false;
}

static bool sight_is_target(const D3D12_SHADER_BYTECODE& ps, const D3D12_SHADER_BYTECODE& vs) {
    return CyberpunkVR_SightPsSwap && ps.pShaderBytecode && ps.BytecodeLength &&
           vs.pShaderBytecode && vs.BytecodeLength &&
           fnv1a(ps.pShaderBytecode, ps.BytecodeLength) == CyberpunkVR_SightPsHash &&
           fnv1a(vs.pShaderBytecode, vs.BytecodeLength) == CyberpunkVR_SightVsHash &&
           sight_blob_ready();
}

// ---- the OUTLINE shader ------------------------------------------------------------------------
//
// VisionMode_Highlight, the compute pass that draws the scanner/focus outline. Replaced so the line
// can be made thicker: its width is nine taps at radius one (the shader's own `c_offsets`), and that
// number exists nowhere else -- not in the scripts, whose whole surface is two colour indices and a
// few flags, and not in any archive, which holds no outline resource at all.
//
// IDENTIFIED BY THE CONTAINER DIGEST, not by an FNV of the bytes: every DXBC/DXIL blob carries its
// own digest at offset 4, and the capture reports it, so the target is known before the game has ever
// run. That is the whole reason this needs no discovery session the way the sight shader did.
// ON, with a self-validating guard rather than a promise: the pipeline is built against the root
// signature the engine has ALREADY bound for this dispatch, so a node that is not the outline pass
// fails creation (its signature does not fit our shader's bindings) and nothing is swapped.
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_VisionCsSwap = 1;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugVisionCsSwaps = 0;
static const uint8_t kVisionCsDigest[16] = {
    0x78, 0x6E, 0x64, 0xC7, 0x28, 0x8B, 0xCE, 0x14,
    0xF7, 0x70, 0x6B, 0x9A, 0x61, 0xC2, 0x64, 0xFF };
static std::vector<uint8_t> g_vision_cs_blob;
static std::atomic<int> g_vision_cs_state{0};   // 0 untried, 1 loaded, -1 missing

static bool vision_cs_blob_ready() {
    int st = g_vision_cs_state.load(std::memory_order_acquire);
    if (st) return st > 0;
    static std::mutex mtx;
    std::lock_guard<std::mutex> lk(mtx);
    st = g_vision_cs_state.load(std::memory_order_relaxed);
    if (st) return st > 0;
    char path[MAX_PATH]{};
    HMODULE self = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&vision_cs_blob_ready), &self) && self &&
        GetModuleFileNameA(self, path, MAX_PATH)) {
        char* slash = strrchr(path, '\\');
        if (slash) {
            *(slash + 1) = 0;
            strcat_s(path, "CyberpunkVR_VisionCs.dxil");
            HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER sz{};
                if (GetFileSizeEx(f, &sz) && sz.QuadPart > 64 && sz.QuadPart < (4 << 20)) {
                    g_vision_cs_blob.resize(static_cast<size_t>(sz.QuadPart));
                    DWORD got = 0;
                    if (!(ReadFile(f, g_vision_cs_blob.data(), static_cast<DWORD>(sz.QuadPart),
                                   &got, nullptr) && got == sz.QuadPart &&
                          memcmp(g_vision_cs_blob.data(), "DXBC", 4) == 0)) {
                        g_vision_cs_blob.clear();
                    }
                }
                CloseHandle(f);
            }
            if (!g_vision_cs_blob.empty()) {
                g_vision_cs_state.store(1, std::memory_order_release);
                log("[pso] outline CS replacement loaded: %zu B", g_vision_cs_blob.size());
                return true;
            }
            log("[pso] outline CS replacement NOT found (%s) -- original kept", path);
        }
    }
    g_vision_cs_state.store(-1, std::memory_order_release);
    return false;
}

static bool vision_cs_is_target(const D3D12_SHADER_BYTECODE& cs) {
    if (!CyberpunkVR_VisionCsSwap || !cs.pShaderBytecode || cs.BytecodeLength < 20) return false;
    const uint8_t* b = static_cast<const uint8_t*>(cs.pShaderBytecode);
    if (memcmp(b, "DXBC", 4) != 0) return false;
    if (memcmp(b + 4, kVisionCsDigest, sizeof(kVisionCsDigest)) != 0) return false;
    return vision_cs_blob_ready();
}

using PFN_CreateComputePipelineState = HRESULT (STDMETHODCALLTYPE*)(
    ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
static PFN_CreateComputePipelineState g_orig_CreateComputePso = nullptr;


// The last compute root signature the engine bound anywhere, kept in CommandListCensus.cpp.
// Needed because the thread-local one is null on lists recorded by other threads.
extern std::atomic<ID3D12RootSignature*> g_last_compute_rootsig;

thread_local ID3D12RootSignature* t_current_compute_rootsig = nullptr;
static ID3D12PipelineState* g_vision_cs_pso = nullptr;
static ID3D12RootSignature* g_vision_cs_pso_rs = nullptr;
static std::mutex g_vision_cs_pso_mtx;

// Built once, from OUR shader and the engine's own compute root signature -- the one it has already
// bound for this dispatch, so every descriptor table the engine set still lands where the shader
// expects it. If the signature changes (a different pipeline ran first) the old one is dropped and a
// new pipeline built, which in practice happens never after the first frame.
ID3D12PipelineState* vision_cs_pso_get(ID3D12GraphicsCommandList* list) {
    if (!CyberpunkVR_VisionCsSwap || !list) return nullptr;
    // ONLY THE ONE BOUND FOR THIS DISPATCH. A pipeline built against a DIFFERENT root signature
    // still binds -- the register-to-descriptor mapping simply comes out wrong -- and the
    // failure looks exactly like what was measured: writes to u0/u1 landed (a paint-red variant
    // flooded the frame) while every SRV read came back zero (a probe painting wherever the
    // flags said 'outlined' painted nothing). So the global fallback is not used here: without
    // the real signature we do not substitute at all.
    ID3D12RootSignature* rs = t_current_compute_rootsig;
    if (!rs || !vision_cs_blob_ready()) return nullptr;
    std::lock_guard<std::mutex> lk(g_vision_cs_pso_mtx);
    if (g_vision_cs_pso && g_vision_cs_pso_rs == rs) return g_vision_cs_pso;
    ID3D12Device* dev = nullptr;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return nullptr;
    if (g_vision_cs_pso) { g_vision_cs_pso->Release(); g_vision_cs_pso = nullptr; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
    d.pRootSignature = rs;
    d.CS.pShaderBytecode = g_vision_cs_blob.data();
    d.CS.BytecodeLength = g_vision_cs_blob.size();
    ID3D12PipelineState* pso = nullptr;
    const HRESULT hr = dev->CreateComputePipelineState(&d, IID_PPV_ARGS(&pso));
    dev->Release();
    if (FAILED(hr) || !pso) {
        // Degrade, never refuse: without our pipeline the engine draws its own thin outline.
        static bool s_said = false;
        if (!s_said) {
            s_said = true;
            log("[pso] outline CS pipeline REFUSED hr=0x%08X rs=%p -- engine's own kept",
                static_cast<unsigned>(hr), (void*)rs);
        }
        return nullptr;
    }
    g_vision_cs_pso = pso;
    g_vision_cs_pso_rs = rs;
    log("[pso] outline CS pipeline built against the engine's root signature rs=%p", (void*)rs);
    return g_vision_cs_pso;
}



static HRESULT STDMETHODCALLTYPE Hook_CreateComputePipelineState(
        ID3D12Device* self, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
        REFIID riid, void** out) {
    if (desc && vision_cs_is_target(desc->CS)) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC d = *desc;
        d.CS.pShaderBytecode = g_vision_cs_blob.data();
        d.CS.BytecodeLength  = g_vision_cs_blob.size();
        HRESULT hr2 = g_orig_CreateComputePso(self, &d, riid, out);
        if (SUCCEEDED(hr2)) {
            if(out&&*out)cvr::menu_preview::RecordComputeShader(reinterpret_cast<uintptr_t>(*out),d.CS);
            CVR_DIAGNOSTIC(++CyberpunkVR_DebugVisionCsSwaps);
            log("[pso] outline CS substituted (compute desc) pso=%p", out ? *out : nullptr);
            return hr2;
        }
        // Degrade, never refuse: a rejected replacement leaves the engine's own outline, which is
        // thin but correct. Losing the pipeline entirely would lose the outline for good.
        log("[pso] outline CS substitution REFUSED hr=0x%08X -- original kept",
            static_cast<unsigned>(hr2));
    }
    const auto result=g_orig_CreateComputePso(self,desc,riid,out);
    if(SUCCEEDED(result)&&desc&&out&&*out)cvr::menu_preview::RecordComputeShader(reinterpret_cast<uintptr_t>(*out),desc->CS);
    return result;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateGraphicsPipelineState(
        ID3D12Device* self, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
        REFIID riid, void** out) {
    D3D12_SHADER_BYTECODE markerVs{};
    if(desc && cvr::markers::VertexShaderReplacement(desc->VS,markerVs)) {
        auto patched=*desc;patched.VS=markerVs;patched.CachedPSO={};
        const auto hr=g_orig_CreateGfxPso(self,&patched,riid,out);
        cvr::markers::VertexShaderResult(desc->VS,SUCCEEDED(hr));
        if(SUCCEEDED(hr)) {
            if(out && *out) pso_ids_record(*out,desc->PS,desc->VS);
            CVR_DIAGNOSTIC(++CyberpunkVR_DebugPsoGfx);return hr;
        }
    }
    if (desc && desc->PS.pShaderBytecode && desc->PS.BytecodeLength &&
        fnv1a(desc->PS.pShaderBytecode, desc->PS.BytecodeLength) == CyberpunkVR_SightPsHash) {
        rootsig_dump(desc->pRootSignature);
    }
    if (desc && sight_is_target(desc->PS, desc->VS)) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = *desc;
        d.PS.pShaderBytecode = g_sight_blob.data();
        d.PS.BytecodeLength = g_sight_blob.size();
        d.VS.pShaderBytecode = g_sight_vs_blob.data();
        d.VS.BytecodeLength = g_sight_vs_blob.size();
        HRESULT hr2 = g_orig_CreateGfxPso(self, &d, riid, out);
        if (SUCCEEDED(hr2)) {
            CVR_DIAGNOSTIC(++CyberpunkVR_DebugSightSwaps);
            log("[pso] sight PS substituted (graphics desc) pso=%p", out ? *out : nullptr);
            if (out && *out) pso_ids_record(*out, desc->PS, desc->VS);  // keep the ORIGINAL id
            CVR_DIAGNOSTIC(++CyberpunkVR_DebugPsoGfx);
            return hr2;
        }
        // A rejected replacement must not cost the game its shader: fall through to the original.
        log("[pso] sight PS substitution REFUSED hr=0x%08X -- keeping the original",
            static_cast<unsigned>(hr2));
    }
    HRESULT hr = g_orig_CreateGfxPso(self, desc, riid, out);
    if (SUCCEEDED(hr) && out && *out && desc) {
        pso_ids_record(*out, desc->PS, desc->VS);
        cvr::stereo::native_probe::GraphicsCreated(self,*desc,static_cast<ID3D12PipelineState*>(*out));
        CVR_DIAGNOSTIC(++CyberpunkVR_DebugPsoGfx);
    }
    return hr;
}

// The stream form is a tagged blob: {alignas(void*) SUBOBJECT_TYPE, payload} repeated. Walking it
// is the only way to see the shaders, and the walk has to respect the pointer alignment between
// entries or it desynchronises and reads garbage.
// Offsets of the PS and VS payloads inside the tagged stream, SIZE_MAX when absent. Kept in its
// own function on purpose: it needs SEH, and SEH cannot share a frame with objects that unwind.
static void pso_stream_find(const uint8_t* p, size_t len, size_t* psoff, size_t* vsoff, size_t* cacheoff=nullptr,size_t* csoff=nullptr) {
    *psoff = SIZE_MAX; *vsoff = SIZE_MAX;
    if(cacheoff) *cacheoff=SIZE_MAX;
    if(csoff) *csoff=SIZE_MAX;
    if (!p || !len) return;
    const uint8_t* base = p;
    const uint8_t* end = p + len;
    __try {
        while (p + sizeof(void*) <= end) {
            const auto type = *reinterpret_cast<const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE*>(p);
            const uint8_t* payload = p + sizeof(void*);
            size_t plen = 0;
            switch (type) {
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
                plen = sizeof(D3D12_SHADER_BYTECODE);
                if (payload + plen > end) { p = end; break; }
                if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS)
                    *psoff = static_cast<size_t>(payload - base);
                else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS)
                    *vsoff = static_cast<size_t>(payload - base);
                else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS && csoff)
                    *csoff = static_cast<size_t>(payload - base);
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
                plen = sizeof(void*); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
                plen = sizeof(D3D12_STREAM_OUTPUT_DESC); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:
                plen = sizeof(D3D12_BLEND_DESC); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
                plen = sizeof(UINT); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:
                plen = sizeof(D3D12_RASTERIZER_DESC); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
                plen = sizeof(D3D12_DEPTH_STENCIL_DESC); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
                plen = sizeof(D3D12_INPUT_LAYOUT_DESC); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
                plen = sizeof(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
                plen = sizeof(D3D12_PRIMITIVE_TOPOLOGY_TYPE); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS:
                plen = sizeof(D3D12_RT_FORMAT_ARRAY); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
                plen = sizeof(DXGI_FORMAT); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
                plen = sizeof(DXGI_SAMPLE_DESC); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
                plen = sizeof(UINT); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
                if(cacheoff && payload+sizeof(D3D12_CACHED_PIPELINE_STATE)<=end)
                    *cacheoff=static_cast<size_t>(payload-base);
                plen = sizeof(D3D12_CACHED_PIPELINE_STATE); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
                plen = sizeof(D3D12_PIPELINE_STATE_FLAGS); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:
                plen = sizeof(D3D12_DEPTH_STENCIL_DESC1); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:
                plen = sizeof(D3D12_VIEW_INSTANCING_DESC); break;
            default:
                // Unknown tag: the walk can no longer be trusted, so stop rather than
                // resynchronise on a guess.
                p = end; plen = 0; break;
            }
            if (p >= end) break;
            p += (sizeof(void*) + plen + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { *psoff = SIZE_MAX; *vsoff = SIZE_MAX;if(csoff)*csoff=SIZE_MAX; }
}


static size_t pso_stream_find_cs(const uint8_t* p, size_t len) {
    size_t csoff = SIZE_MAX;
    if (!p || !len) return csoff;
    const uint8_t* base = p;
    const uint8_t* end = p + len;
    __try {
        while (p + sizeof(void*) <= end) {
            const auto type = *reinterpret_cast<const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE*>(p);
            const uint8_t* payload = p + sizeof(void*);
            size_t plen = 0;
            switch (type) {
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
                plen = sizeof(D3D12_SHADER_BYTECODE);
                if (payload + plen > end) { p = end; break; }
                if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS)
                    csoff = static_cast<size_t>(payload - base);
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
                plen = sizeof(void*); break;
            default:
                // Anything whose size is not known here ends the walk rather than guessing a
                // stride: a wrong stride walks off into the caller's memory.
                p = end; plen = 0; break;
            }
            if (p >= end) break;
            const size_t step = ((sizeof(void*) + plen) + (sizeof(void*) - 1)) & ~(sizeof(void*) - 1);
            if (!step) break;
            p += step;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { csoff = SIZE_MAX; }
    return csoff;
}

static HRESULT STDMETHODCALLTYPE Hook_CreatePipelineState(
        ID3D12Device* self, const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
        REFIID riid, void** out) {
    // Same substitution as the classic path, but the stream is the caller's memory, so it is
    // COPIED first and the copy is patched -- writing into the engine's own description would
    // outlive this call and be visible to whatever else reads it.
    if (desc && desc->pPipelineStateSubobjectStream && desc->SizeInBytes) {
        const uint8_t* csrc = static_cast<const uint8_t*>(desc->pPipelineStateSubobjectStream);
        const size_t csoff = pso_stream_find_cs(csrc, desc->SizeInBytes);
        if (csoff != SIZE_MAX) {
            const auto& cbc = *reinterpret_cast<const D3D12_SHADER_BYTECODE*>(csrc + csoff);
            if (vision_cs_is_target(cbc)) {
                std::vector<uint8_t> copy(csrc, csrc + desc->SizeInBytes);
                auto& ncbc = *reinterpret_cast<D3D12_SHADER_BYTECODE*>(copy.data() + csoff);
                ncbc.pShaderBytecode = g_vision_cs_blob.data();
                ncbc.BytecodeLength = g_vision_cs_blob.size();
                D3D12_PIPELINE_STATE_STREAM_DESC nd = *desc;
                nd.pPipelineStateSubobjectStream = copy.data();
                HRESULT hrc = g_orig_CreatePso(self, &nd, riid, out);
                if (SUCCEEDED(hrc)) {
                    if(out&&*out)cvr::menu_preview::RecordComputeShader(reinterpret_cast<uintptr_t>(*out),ncbc);
                    CVR_DIAGNOSTIC(++CyberpunkVR_DebugVisionCsSwaps);
                    log("[pso] outline CS substituted (stream desc) pso=%p", out ? *out : nullptr);
                    return hrc;
                }
                log("[pso] outline CS substitution REFUSED hr=0x%08X (stream) -- original kept",
                    static_cast<unsigned>(hrc));
            }
        }
        const uint8_t* src = static_cast<const uint8_t*>(desc->pPipelineStateSubobjectStream);
        size_t psoff = SIZE_MAX, vsoff = SIZE_MAX, cacheoff=SIZE_MAX;
        pso_stream_find(src, desc->SizeInBytes, &psoff, &vsoff,&cacheoff);
        if(vsoff!=SIZE_MAX) {
            const auto& originalVs=*reinterpret_cast<const D3D12_SHADER_BYTECODE*>(src+vsoff);
            D3D12_SHADER_BYTECODE markerVs{};
            if(cvr::markers::VertexShaderReplacement(originalVs,markerVs)) {
                std::vector<uint8_t> copy(src,src+desc->SizeInBytes);
                *reinterpret_cast<D3D12_SHADER_BYTECODE*>(copy.data()+vsoff)=markerVs;
                if(cacheoff!=SIZE_MAX) *reinterpret_cast<D3D12_CACHED_PIPELINE_STATE*>(copy.data()+cacheoff)={};
                auto patched=*desc;patched.pPipelineStateSubobjectStream=copy.data();
                const auto hr=g_orig_CreatePso(self,&patched,riid,out);
                cvr::markers::VertexShaderResult(originalVs,SUCCEEDED(hr));
                if(SUCCEEDED(hr)) {
                    const auto ps=psoff!=SIZE_MAX ? *reinterpret_cast<const D3D12_SHADER_BYTECODE*>(src+psoff) : D3D12_SHADER_BYTECODE{};
                    if(out && *out) pso_ids_record(*out,ps,originalVs);
                    CVR_DIAGNOSTIC(++CyberpunkVR_DebugPsoStream);return hr;
                }
            }
        }
        if (psoff != SIZE_MAX && vsoff != SIZE_MAX) {
            const auto& bc = *reinterpret_cast<const D3D12_SHADER_BYTECODE*>(src + psoff);
            const auto& bcv = *reinterpret_cast<const D3D12_SHADER_BYTECODE*>(src + vsoff);
            if (sight_is_target(bc, bcv)) {
                std::vector<uint8_t> copy(src, src + desc->SizeInBytes);
                auto& nbc = *reinterpret_cast<D3D12_SHADER_BYTECODE*>(copy.data() + psoff);
                nbc.pShaderBytecode = g_sight_blob.data();
                nbc.BytecodeLength = g_sight_blob.size();
                auto& nbv = *reinterpret_cast<D3D12_SHADER_BYTECODE*>(copy.data() + vsoff);
                nbv.pShaderBytecode = g_sight_vs_blob.data();
                nbv.BytecodeLength = g_sight_vs_blob.size();
                D3D12_PIPELINE_STATE_STREAM_DESC nd = *desc;
                nd.pPipelineStateSubobjectStream = copy.data();
                HRESULT hr2 = g_orig_CreatePso(self, &nd, riid, out);
                if (SUCCEEDED(hr2)) {
                    CVR_DIAGNOSTIC(++CyberpunkVR_DebugSightSwaps);
                    CVR_DIAGNOSTIC(++CyberpunkVR_DebugPsoStream);
                    log("[pso] sight PS substituted (stream desc) pso=%p", out ? *out : nullptr);
                    return hr2;
                }
                log("[pso] sight PS substitution REFUSED hr=0x%08X (stream) -- original kept",
                    static_cast<unsigned>(hr2));
            }
        }
    }
    HRESULT hr = g_orig_CreatePso(self, desc, riid, out);
    if (SUCCEEDED(hr) && out && *out && desc && desc->pPipelineStateSubobjectStream) {
        size_t psoff = SIZE_MAX, vsoff = SIZE_MAX,csoff=SIZE_MAX;
        pso_stream_find(static_cast<const uint8_t*>(desc->pPipelineStateSubobjectStream),
                        desc->SizeInBytes, &psoff, &vsoff,nullptr,&csoff);
        D3D12_SHADER_BYTECODE ps{}, vs{};
        const uint8_t* base = static_cast<const uint8_t*>(desc->pPipelineStateSubobjectStream);
        if (psoff != SIZE_MAX) ps = *reinterpret_cast<const D3D12_SHADER_BYTECODE*>(base + psoff);
        if (vsoff != SIZE_MAX) vs = *reinterpret_cast<const D3D12_SHADER_BYTECODE*>(base + vsoff);
        if (ps.pShaderBytecode || vs.pShaderBytecode) pso_ids_record(*out, ps, vs);
        if(csoff!=SIZE_MAX)cvr::menu_preview::RecordComputeShader(reinterpret_cast<uintptr_t>(*out),*reinterpret_cast<const D3D12_SHADER_BYTECODE*>(base+csoff));
        cvr::stereo::native_probe::StreamCreated(self,*desc,static_cast<ID3D12PipelineState*>(*out));
        CVR_DIAGNOSTIC(++CyberpunkVR_DebugPsoStream);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateCommandList(
        ID3D12Device* self, UINT node_mask, D3D12_COMMAND_LIST_TYPE type,
        ID3D12CommandAllocator* allocator, ID3D12PipelineState* initial_state,
        REFIID riid, void** out) {
    HRESULT hr = g_orig_CreateCommandList(
        self, node_mask, type, allocator, initial_state, riid, out);
    if(SUCCEEDED(hr)&&out&&*out&&!cvr::gpu::RegisterNativeObject(static_cast<ID3D12CommandList*>(*out)))
        log("[exec] native list identity registration failed list=%p",*out);
    const void* const caller = _ReturnAddress();
    // COMPUTE as well as DIRECT. Registering only DIRECT lists left the engine's async-compute
    // list (`AsyncComputeDuringShadowmaps` in a capture) invisible to every hook here -- the
    // dispatch census, the ExecuteIndirect census, the barrier probes, all of them. That blind
    // spot is why the three 6x6x6 grading-volume builds each view does per frame never showed up
    // live even though both captures contain them.
    //
    // Safe to widen: command_list_hook_entry() keys on the VTABLE, not on the list pointer, so a
    // compute list simply finds the compute vtable's entry and the original is always called.
    // (A compute list has its own vtable, which is exactly why it was never patched before.)
    if (SUCCEEDED(hr) && out && *out &&
        (type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_COMPUTE)) {
        patch_command_list_vtable(*out);
        if(!cvr::menu_preview::EditorUiPrivateCommands()){
            if(cvr::menu_preview::UiConsumerEnabled()&&CyberpunkVR_NativeStereoProbeState.load(std::memory_order_relaxed)!=1)
                cvr::stereo::scene_state::Reset(reinterpret_cast<ID3D12GraphicsCommandList*>(*out));
            cvr::menu_preview::UiPipelineState(reinterpret_cast<ID3D12GraphicsCommandList*>(*out),initial_state);
        }
    }
    if (SUCCEEDED(hr) && out && *out && type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        CetOverlayNoteCommandListCreated(reinterpret_cast<ID3D12GraphicsCommandList*>(*out), caller);
    }
    return hr;
}

enum class UiBeginKind {Srv,Copy};
struct UiBeginSample {std::atomic<uint64_t> calls{},ticks{},maximum{};};
struct UiBeginTiming {LARGE_INTEGER frequency{};std::atomic<int64_t> lastReport{};std::atomic<uint32_t> reports{};UiBeginSample srv,copy;UiBeginTiming(){QueryPerformanceFrequency(&frequency);}};
static UiBeginTiming& UiTiming(){static auto* state=new UiBeginTiming;return *state;}
static bool UiTimingEnabled(){static const bool enabled=[] {char value[4]{};return GetEnvironmentVariableA("CPVR_MENU_UI_TIMING",value,sizeof(value))==1&&value[0]=='1';}();return enabled;}
static void NoteUiBegin(UiBeginKind kind,uint64_t ticks,int64_t now){
    auto& state=UiTiming();auto& sample=kind==UiBeginKind::Srv?state.srv:state.copy;
    sample.calls.fetch_add(1,std::memory_order_relaxed);sample.ticks.fetch_add(ticks,std::memory_order_relaxed);
    auto maximum=sample.maximum.load(std::memory_order_relaxed);while(maximum<ticks&&!sample.maximum.compare_exchange_weak(maximum,ticks,std::memory_order_relaxed)){}
    if(!state.frequency.QuadPart||state.reports.load(std::memory_order_relaxed)>=180)return;
    auto previous=state.lastReport.load(std::memory_order_relaxed);
    if(!previous){state.lastReport.compare_exchange_strong(previous,now,std::memory_order_relaxed);return;}
    if(now-previous<state.frequency.QuadPart*10||!state.lastReport.compare_exchange_strong(previous,now,std::memory_order_relaxed))return;
    if(state.reports.fetch_add(1,std::memory_order_relaxed)>=180)return;
    const auto srvCalls=state.srv.calls.exchange(0),srvTicks=state.srv.ticks.exchange(0),srvMax=state.srv.maximum.exchange(0);
    const auto copyCalls=state.copy.calls.exchange(0),copyTicks=state.copy.ticks.exchange(0),copyMax=state.copy.maximum.exchange(0);
    const auto scope=cvr::menu_preview::UiConsumerTimingScope();
    Log("[MenuPreviewUI] ui-begin-timing intervalMs=%llu scope=%u activeAlias=%u worldOrNoPreview=%u srvCalls=%llu srvTotalTicks=%llu srvMaxTicks=%llu copyCalls=%llu copyTotalTicks=%llu copyMaxTicks=%llu qpcFrequency=%lld; Begin-only QPC, original D3D12 call excluded\n",uint64_t((now-previous)*1000/state.frequency.QuadPart),scope,unsigned(scope==2),unsigned(scope==0),srvCalls,srvTicks,srvMax,copyCalls,copyTicks,copyMax,state.frequency.QuadPart);
}
template<class F> static auto TimedUiBegin(UiBeginKind kind,F&& begin){
    LARGE_INTEGER start{},end{};bool timed=false;
    try{timed=UiTimingEnabled()&&QueryPerformanceCounter(&start);}catch(...){timed=false;}
    auto token=begin(); // The original observer runs exactly once; its exception policy is unchanged.
    if(timed)try{if(QueryPerformanceCounter(&end))NoteUiBegin(kind,uint64_t(end.QuadPart-start.QuadPart),end.QuadPart);}catch(...){/* Diagnostics cannot interrupt the native call chain. */}
    return token;
}
using PreviewCopyDescriptorsFn=void (STDMETHODCALLTYPE*)(ID3D12Device*,UINT,const D3D12_CPU_DESCRIPTOR_HANDLE*,const UINT*,UINT,const D3D12_CPU_DESCRIPTOR_HANDLE*,const UINT*,D3D12_DESCRIPTOR_HEAP_TYPE);
using PreviewCopySimpleFn=void (STDMETHODCALLTYPE*)(ID3D12Device*,UINT,D3D12_CPU_DESCRIPTOR_HANDLE,D3D12_CPU_DESCRIPTOR_HANDLE,D3D12_DESCRIPTOR_HEAP_TYPE);
static PreviewCopyDescriptorsFn previewCopyOriginal{};
static PreviewCopySimpleFn previewCopySimpleOriginal{};
static void STDMETHODCALLTYPE PreviewCopyDescriptors(ID3D12Device* self,UINT destinations,const D3D12_CPU_DESCRIPTOR_HANDLE* destinationStarts,const UINT* destinationSizes,UINT sources,const D3D12_CPU_DESCRIPTOR_HANDLE* sourceStarts,const UINT* sourceSizes,D3D12_DESCRIPTOR_HEAP_TYPE type){
    const auto token=cvr::menu_preview::BeginPreviewDescriptorCopies(self,destinations,destinationStarts,destinationSizes,type);
    const auto uiToken=TimedUiBegin(UiBeginKind::Copy,[&]{return cvr::menu_preview::BeginUiDescriptorCopy(self,destinations,destinationStarts,destinationSizes,sources,sourceStarts,sourceSizes,type);});
    previewCopyOriginal(self,destinations,destinationStarts,destinationSizes,sources,sourceStarts,sourceSizes,type);
    cvr::menu_preview::FinishUiDescriptorCopy(uiToken);
    cvr::menu_preview::FinishPreviewDescriptorCopies(token);
}
static void STDMETHODCALLTYPE PreviewCopyDescriptorsSimple(ID3D12Device* self,UINT count,D3D12_CPU_DESCRIPTOR_HANDLE destination,D3D12_CPU_DESCRIPTOR_HANDLE source,D3D12_DESCRIPTOR_HEAP_TYPE type){
    const auto token=cvr::menu_preview::BeginPreviewDescriptorCopies(self,1,&destination,&count,type);
    const auto uiToken=TimedUiBegin(UiBeginKind::Copy,[&]{return cvr::menu_preview::BeginUiDescriptorCopy(self,1,&destination,&count,1,&source,&count,type);});
    previewCopySimpleOriginal(self,count,destination,source,type);
    cvr::menu_preview::FinishUiDescriptorCopy(uiToken);
    cvr::menu_preview::FinishPreviewDescriptorCopies(token);
}
using UiCreateSrvFn=void(STDMETHODCALLTYPE*)(ID3D12Device*,ID3D12Resource*,const D3D12_SHADER_RESOURCE_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE);
using UiCreateUavFn=void(STDMETHODCALLTYPE*)(ID3D12Device*,ID3D12Resource*,ID3D12Resource*,const D3D12_UNORDERED_ACCESS_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE);
static UiCreateSrvFn uiCreateSrvOriginal{};static UiCreateUavFn uiCreateUavOriginal{};
static void STDMETHODCALLTYPE UiCreateSrv(ID3D12Device* device,ID3D12Resource* resource,const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE handle){
    const auto token=TimedUiBegin(UiBeginKind::Srv,[&]{return cvr::menu_preview::BeginUiSrvWrite(handle);});uiCreateSrvOriginal(device,resource,desc,handle);
    cvr::menu_preview::FinishUiSrvWrite(token,resource,handle);
}
static void STDMETHODCALLTYPE UiCreateUav(ID3D12Device* device,ID3D12Resource* resource,ID3D12Resource* counter,const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE handle){
    const auto token=cvr::menu_preview::BeginUiSrvWrite(handle);uiCreateUavOriginal(device,resource,counter,desc,handle);
    cvr::menu_preview::FinishUiSrvWrite(token,resource,handle,true);
}
void patch_device_descriptor_slot(void* device) {
    if(device&&!cvr::gpu::RegisterNativeObject(static_cast<ID3D12Device*>(device)))
        log("[exec] native device identity registration failed device=%p",device);
    if (!device) return;
    bool expected = false;
    if (!g_desc_vtable_patched.compare_exchange_strong(expected, true)) return;
    void** vt = *reinterpret_cast<void***>(device);
    // ID3D12Device slot 14 = CreateDescriptorHeap.
    DWORD oldp = 0;
    if (VirtualProtect(&vt[14], sizeof(void*), PAGE_READWRITE, &oldp)) {
        g_orig_CreateDescriptorHeap =
            reinterpret_cast<PFN_CreateDescriptorHeap>(vt[14]);
        vt[14] = reinterpret_cast<void*>(&Hook_CreateDescriptorHeap);
        DWORD junk = 0;
        VirtualProtect(&vt[14], sizeof(void*), oldp, &junk);
        log("[descheap] CreateDescriptorHeap hooked (dev=%p vt=%p orig=%p)",
            device, (void*)vt, (void*)g_orig_CreateDescriptorHeap);
    } else {
        g_desc_vtable_patched.store(false, std::memory_order_release);
        log("[descheap] FAILED to patch device vtable slot 14");
    }
    if(cvr::menu_preview::PreviewMetadataRequested()){
        DWORD protection{};if(VirtualProtect(vt+23,2*sizeof(void*),PAGE_READWRITE,&protection)){
            previewCopyOriginal=reinterpret_cast<PreviewCopyDescriptorsFn>(vt[23]);
            previewCopySimpleOriginal=reinterpret_cast<PreviewCopySimpleFn>(vt[24]);
            InterlockedExchangePointer(reinterpret_cast<void* volatile*>(vt+23),reinterpret_cast<void*>(&PreviewCopyDescriptors));
            InterlockedExchangePointer(reinterpret_cast<void* volatile*>(vt+24),reinterpret_cast<void*>(&PreviewCopyDescriptorsSimple));
            DWORD ignored{};VirtualProtect(vt+23,2*sizeof(void*),protection,&ignored);
        }else cvr::menu_preview::BeginPreviewDescriptorCopies(reinterpret_cast<ID3D12Device*>(device),1,nullptr,nullptr,D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }
    if(cvr::menu_preview::UiStereoRequested()){
        DWORD protection{};
        if(VirtualProtect(vt+18,2*sizeof(void*),PAGE_READWRITE,&protection)){
            uiCreateSrvOriginal=reinterpret_cast<UiCreateSrvFn>(vt[18]);uiCreateUavOriginal=reinterpret_cast<UiCreateUavFn>(vt[19]);
            InterlockedExchangePointer(reinterpret_cast<void* volatile*>(vt+18),reinterpret_cast<void*>(&UiCreateSrv));
            InterlockedExchangePointer(reinterpret_cast<void* volatile*>(vt+19),reinterpret_cast<void*>(&UiCreateUav));
            DWORD ignored{};VirtualProtect(vt+18,2*sizeof(void*),protection,&ignored);
        }
    }
    // ID3D12Device slot 8 = CreateCommandQueue (capture queues -> hook execute).
    if (g_enable_exec_probe) {
        DWORD o8 = 0;
        if (VirtualProtect(&vt[8], sizeof(void*), PAGE_READWRITE, &o8)) {
            g_orig_CreateCommandQueue = reinterpret_cast<PFN_CreateCommandQueue>(vt[8]);
            vt[8] = reinterpret_cast<void*>(&Hook_CreateCommandQueue);
            DWORD junk = 0;
            VirtualProtect(&vt[8], sizeof(void*), o8, &junk);
            log("[exec] CreateCommandQueue hooked (dev=%p)", device);
        }
    }
    // Slot 12 = CreateCommandList. Track each distinct D3D12Core command-list
    // vtable so OMSetRenderTargets can map the RTV bound specifically inside the
    // VRCAM CopyToTexture node. Slot 9 is CreateCommandAllocator (never patch it).
    DWORD o12 = 0;
    if (VirtualProtect(&vt[12], sizeof(void*), PAGE_READWRITE, &o12)) {
        g_orig_CreateCommandList = reinterpret_cast<PFN_CreateCommandList>(vt[12]);
        vt[12] = reinterpret_cast<void*>(&Hook_CreateCommandList);
        DWORD junk = 0;
        VirtualProtect(&vt[12], sizeof(void*), o12, &junk);
        log("[mirror] real device CreateCommandList hooked dev=%p orig=%p",
            device, (void*)g_orig_CreateCommandList);
    }
    // Slot 10 = CreateGraphicsPipelineState. Slot 47 = ID3D12Device2::CreatePipelineState, and
    // it is only touched when the device actually implements ID3D12Device2 -- patching a vtable
    // slot that may not exist is how a hook corrupts the object next to it.
    for (int k = 0; k < 2; ++k) {
        const UINT slot = k ? 29u : 27u;
        DWORD oldp = 0;
        if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_READWRITE, &oldp)) continue;
        if (k) {
            g_orig_CreatePlaced = reinterpret_cast<PFN_CreatePlacedResource>(vt[slot]);
            vt[slot] = reinterpret_cast<void*>(&Hook_CreatePlacedResource);
        } else {
            g_orig_CreateCommitted = reinterpret_cast<PFN_CreateCommittedResource>(vt[slot]);
            vt[slot] = reinterpret_cast<void*>(&Hook_CreateCommittedResource);
        }
        DWORD junk = 0; VirtualProtect(&vt[slot], sizeof(void*), oldp, &junk);
    }
    log("[sightaxis] buffer-address map hooked dev=%p", device);
    if(cvr::menu_preview::UiConsumerEnabled()){
        // Interface version is queried before accessing its documented appended slots.
        auto* base=static_cast<ID3D12Device*>(device);
        HookCommittedAllocationMethod<ID3D12Device4>(base,53,g_orig_CreateCommitted1,&Hook_CreateCommitted1);
        HookCommittedAllocationMethod<ID3D12Device8>(base,69,g_orig_CreateCommitted2,&Hook_CreateCommitted2);
        HookCommittedAllocationMethod<ID3D12Device10>(base,76,g_orig_CreateCommitted3,&Hook_CreateCommitted3);
    }
    DWORD o16 = 0;
    if (VirtualProtect(&vt[16], sizeof(void*), PAGE_READWRITE, &o16)) {
        g_orig_CreateRootSig = reinterpret_cast<PFN_CreateRootSignature>(vt[16]);
        vt[16] = reinterpret_cast<void*>(&Hook_CreateRootSignature);
        DWORD junk = 0;
        VirtualProtect(&vt[16], sizeof(void*), o16, &junk);
        log("[rootsig] CreateRootSignature hooked dev=%p", device);
    }
    DWORD o10 = 0;
    if (VirtualProtect(&vt[10], sizeof(void*), PAGE_READWRITE, &o10)) {
        g_orig_CreateGfxPso = reinterpret_cast<PFN_CreateGraphicsPipelineState>(vt[10]);
        vt[10] = reinterpret_cast<void*>(&Hook_CreateGraphicsPipelineState);
        DWORD junk = 0;
        VirtualProtect(&vt[10], sizeof(void*), o10, &junk);
        log("[pso] CreateGraphicsPipelineState hooked dev=%p", device);
    }
    // Slot 11 = CreateComputePipelineState, where the outline shader is built.
    DWORD o11 = 0;
    if (VirtualProtect(&vt[11], sizeof(void*), PAGE_READWRITE, &o11)) {
        g_orig_CreateComputePso = reinterpret_cast<PFN_CreateComputePipelineState>(vt[11]);
        vt[11] = reinterpret_cast<void*>(&Hook_CreateComputePipelineState);
        DWORD junk = 0;
        VirtualProtect(&vt[11], sizeof(void*), o11, &junk);
        log("[pso] CreateComputePipelineState hooked dev=%p", device);
    }
    {
        ID3D12Device2* dev2 = nullptr;
        auto* dev0 = static_cast<ID3D12Device*>(device);
        if (SUCCEEDED(dev0->QueryInterface(IID_PPV_ARGS(&dev2))) && dev2) {
            DWORD o47 = 0;
            if (VirtualProtect(&vt[47], sizeof(void*), PAGE_READWRITE, &o47)) {
                g_orig_CreatePso = reinterpret_cast<PFN_CreatePipelineState>(vt[47]);
                vt[47] = reinterpret_cast<void*>(&Hook_CreatePipelineState);
                DWORD junk = 0;
                VirtualProtect(&vt[47], sizeof(void*), o47, &junk);
                log("[pso] ID3D12Device2::CreatePipelineState hooked dev=%p", device);
            }
            dev2->Release();
        }
    }
    DWORD o17 = 0;
    if (VirtualProtect(&vt[17], sizeof(void*), PAGE_READWRITE, &o17)) {
        g_orig_CreateCBV = reinterpret_cast<CreateCBVFn>(vt[17]);
        vt[17] = reinterpret_cast<void*>(&hk_CreateCBV);
        DWORD junk = 0;
        VirtualProtect(&vt[17], sizeof(void*), o17, &junk);
        log("[cbv] CreateConstantBufferView hooked dev=%p orig=%p", device,
            (void*)g_orig_CreateCBV);
    }
    // Real game objects only: no throwaway device through sl.interposer.
    DWORD o20 = 0;
    if (VirtualProtect(&vt[20], sizeof(void*), PAGE_READWRITE, &o20)) {
        g_orig_CreateRTV = reinterpret_cast<CreateRTVFn>(vt[20]);
        vt[20] = reinterpret_cast<void*>(&hk_CreateRTV);
        DWORD junk = 0;
        VirtualProtect(&vt[20], sizeof(void*), o20, &junk);
        g_rtv_hook_installed.store(true, std::memory_order_release);
        log("[mirror] real device CreateRTV hooked dev=%p orig=%p",
            device, (void*)g_orig_CreateRTV);
    }
}

}  // namespace detail
}  // namespace cvr

namespace cvr::menu_preview {
bool ResolveComputeLocations(uintptr_t root,const ComputeShaderEvidence& shader,std::array<ComputeLocation,24>& out,uint32_t& count)noexcept {
    out={};count=0;
    if(!shader.bindingsKnown||shader.srvBindingCount>shader.srvBindings.size()||shader.uavBindingCount>shader.uavBindings.size())return false;
    try {
        std::vector<uint8_t> blob;
        {std::lock_guard lock(detail::g_rootsig_mtx);const auto found=detail::g_rootsig_blobs.find(reinterpret_cast<void*>(root));if(found==detail::g_rootsig_blobs.end())return false;blob=found->second;}
        Microsoft::WRL::ComPtr<ID3D12VersionedRootSignatureDeserializer> deserializer;
        if(FAILED(D3D12CreateVersionedRootSignatureDeserializer(blob.data(),blob.size(),IID_PPV_ARGS(&deserializer))))return false;
        const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc{};
        if(FAILED(deserializer->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1,&desc))||!desc||desc->Desc_1_1.NumParameters>64)return false;
        std::array<ComputeLocation,24> resolved{};uint32_t used{};
        const auto bindings=shader.srvBindingCount+shader.uavBindingCount;
        for(uint32_t s=0;s<bindings;++s){const bool uav=s>=shader.srvBindingCount;
            const auto& binding=uav?shader.uavBindings[s-shader.srvBindingCount]:shader.srvBindings[s];
            if(binding.resourceKind!=2||binding.firstRegister!=binding.lastRegister)continue;
            bool matched=false;ComputeLocation location;
            for(UINT r=0;r<desc->Desc_1_1.NumParameters;++r){const auto& parameter=desc->Desc_1_1.pParameters[r];
                if(parameter.ParameterType!=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE||parameter.ShaderVisibility!=D3D12_SHADER_VISIBILITY_ALL)continue;
                uint64_t next{};
                for(UINT i=0;i<parameter.DescriptorTable.NumDescriptorRanges;++i){const auto& range=parameter.DescriptorTable.pDescriptorRanges[i];
                    const uint64_t offset=range.OffsetInDescriptorsFromTableStart==D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND?next:range.OffsetInDescriptorsFromTableStart;next=offset+range.NumDescriptors;
                    if(range.RangeType!=(uav?D3D12_DESCRIPTOR_RANGE_TYPE_UAV:D3D12_DESCRIPTOR_RANGE_TYPE_SRV)||range.RegisterSpace!=binding.registerSpace||binding.firstRegister<range.BaseShaderRegister)continue;
                    const uint64_t relative=uint64_t(binding.firstRegister)-range.BaseShaderRegister;if(relative>=range.NumDescriptors)continue;
                    if(matched||offset+relative>UINT32_MAX)return false;matched=true;location={r,uint32_t(offset+relative),uav};
                }
            }
            if(!matched||used==resolved.size())return false;resolved[used++]=location;
        }
        out=resolved;count=used;return true;
    }catch(...){return false;}
}
bool ResolvePixelSrvLocations(uintptr_t root,const PipelineShaderEvidence& shader,std::array<ShaderSrvLocation,16>& out,uint32_t& count)noexcept {
    out={};count=0;
    if(!shader.srvBindingsKnown||shader.srvBindingCount>shader.srvBindings.size())return false;
    try {
        std::vector<uint8_t> blob;
        {std::lock_guard lock(detail::g_rootsig_mtx);const auto found=detail::g_rootsig_blobs.find(reinterpret_cast<void*>(root));if(found==detail::g_rootsig_blobs.end())return false;blob=found->second;}
        Microsoft::WRL::ComPtr<ID3D12VersionedRootSignatureDeserializer> deserializer;
        if(FAILED(D3D12CreateVersionedRootSignatureDeserializer(blob.data(),blob.size(),IID_PPV_ARGS(&deserializer))))return false;
        const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc{};
        if(FAILED(deserializer->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1,&desc))||!desc||desc->Desc_1_1.NumParameters>64)return false;
        for(uint32_t s=0;s<shader.srvBindingCount;++s){const auto& binding=shader.srvBindings[s];
            if(binding.resourceKind!=2||binding.firstRegister!=binding.lastRegister)continue;
            bool matched=false;ShaderSrvLocation location;
            for(UINT r=0;r<desc->Desc_1_1.NumParameters;++r){const auto& parameter=desc->Desc_1_1.pParameters[r];
                if(parameter.ParameterType!=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE||(parameter.ShaderVisibility!=D3D12_SHADER_VISIBILITY_PIXEL&&parameter.ShaderVisibility!=D3D12_SHADER_VISIBILITY_ALL))continue;
                uint64_t next{};
                for(UINT i=0;i<parameter.DescriptorTable.NumDescriptorRanges;++i){const auto& range=parameter.DescriptorTable.pDescriptorRanges[i];
                    const uint64_t offset=range.OffsetInDescriptorsFromTableStart==D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND?next:range.OffsetInDescriptorsFromTableStart;next=offset+range.NumDescriptors;
                    if(range.RangeType!=D3D12_DESCRIPTOR_RANGE_TYPE_SRV||range.RegisterSpace!=binding.registerSpace||binding.firstRegister<range.BaseShaderRegister)continue;
                    const uint64_t relative=uint64_t(binding.firstRegister)-range.BaseShaderRegister;if(relative>=range.NumDescriptors)continue;
                    if(matched||offset+relative>UINT32_MAX)return false;matched=true;location={r,uint32_t(offset+relative)};
                }
            }
            if(!matched)return false;if(count==out.size())return false;out[count++]=location;
        }
        return true;
    }catch(...) {out={};count=0;return false;}
}
bool ResolveVertexConstantLocation(uintptr_t root,uint32_t shaderRegister,uint32_t registerSpace,VertexConstantLocation& out)noexcept {
    out={};
    try {
        std::vector<uint8_t> blob;
        {std::lock_guard lock(detail::g_rootsig_mtx);const auto found=detail::g_rootsig_blobs.find(reinterpret_cast<void*>(root));if(found==detail::g_rootsig_blobs.end())return false;blob=found->second;}
        Microsoft::WRL::ComPtr<ID3D12VersionedRootSignatureDeserializer> deserializer;
        if(FAILED(D3D12CreateVersionedRootSignatureDeserializer(blob.data(),blob.size(),IID_PPV_ARGS(&deserializer))))return false;
        const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc{};
        if(FAILED(deserializer->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1,&desc))||!desc||desc->Desc_1_1.NumParameters>64||
           (desc->Desc_1_1.Flags&D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS))return false;
        VertexConstantLocation resolved;bool matched=false;
        for(UINT index=0;index<desc->Desc_1_1.NumParameters;++index){const auto& parameter=desc->Desc_1_1.pParameters[index];
            if(parameter.ShaderVisibility!=D3D12_SHADER_VISIBILITY_ALL&&parameter.ShaderVisibility!=D3D12_SHADER_VISIBILITY_VERTEX)continue;
            if(parameter.ParameterType==D3D12_ROOT_PARAMETER_TYPE_CBV&&parameter.Descriptor.ShaderRegister==shaderRegister&&parameter.Descriptor.RegisterSpace==registerSpace){
                if(matched)return false;matched=true;resolved={index,0,false};
            }else if(parameter.ParameterType==D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS&&parameter.Constants.ShaderRegister==shaderRegister&&parameter.Constants.RegisterSpace==registerSpace)return false;
            else if(parameter.ParameterType==D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE){uint64_t next{};
                for(UINT r=0;r<parameter.DescriptorTable.NumDescriptorRanges;++r){const auto& range=parameter.DescriptorTable.pDescriptorRanges[r];
                    const uint64_t offset=range.OffsetInDescriptorsFromTableStart==D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND?next:range.OffsetInDescriptorsFromTableStart;
                    if(offset>UINT64_MAX-range.NumDescriptors)return false;next=offset+range.NumDescriptors;
                    if(range.RangeType!=D3D12_DESCRIPTOR_RANGE_TYPE_CBV||range.RegisterSpace!=registerSpace||shaderRegister<range.BaseShaderRegister)continue;
                    const uint64_t relative=uint64_t(shaderRegister)-range.BaseShaderRegister;
                    if(relative>=range.NumDescriptors)continue;
                    if(matched||offset>UINT32_MAX||relative>UINT32_MAX-offset)return false;
                    matched=true;resolved={index,offset+relative,true};
                }
            }
        }
        if(!matched)return false;out=resolved;return true;
    }catch(...){return false;}
}
bool ResolveVertexConstantRoot(uintptr_t root,uint32_t shaderRegister,uint32_t registerSpace,uint32_t& rootIndex)noexcept {
    rootIndex=UINT32_MAX;
    try {
        std::vector<uint8_t> blob;
        {std::lock_guard lock(detail::g_rootsig_mtx);const auto found=detail::g_rootsig_blobs.find(reinterpret_cast<void*>(root));if(found==detail::g_rootsig_blobs.end())return false;blob=found->second;}
        Microsoft::WRL::ComPtr<ID3D12VersionedRootSignatureDeserializer> deserializer;
        if(FAILED(D3D12CreateVersionedRootSignatureDeserializer(blob.data(),blob.size(),IID_PPV_ARGS(&deserializer))))return false;
        const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc{};
        if(FAILED(deserializer->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1,&desc))||!desc||desc->Desc_1_1.NumParameters>64||
           (desc->Desc_1_1.Flags&D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS))return false;
        uint32_t resolved=UINT32_MAX;
        for(UINT index=0;index<desc->Desc_1_1.NumParameters;++index){const auto& parameter=desc->Desc_1_1.pParameters[index];
            if(parameter.ShaderVisibility!=D3D12_SHADER_VISIBILITY_ALL&&parameter.ShaderVisibility!=D3D12_SHADER_VISIBILITY_VERTEX)continue;
            if(parameter.ParameterType==D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE){
                for(UINT rangeIndex=0;rangeIndex<parameter.DescriptorTable.NumDescriptorRanges;++rangeIndex){const auto& range=parameter.DescriptorTable.pDescriptorRanges[rangeIndex];
                    if(range.RangeType==D3D12_DESCRIPTOR_RANGE_TYPE_CBV&&range.RegisterSpace==registerSpace&&shaderRegister>=range.BaseShaderRegister&&
                       uint64_t(shaderRegister)-range.BaseShaderRegister<range.NumDescriptors)return false;
                }
                continue;
            }
            if(parameter.ParameterType!=D3D12_ROOT_PARAMETER_TYPE_CBV||parameter.Descriptor.ShaderRegister!=shaderRegister||parameter.Descriptor.RegisterSpace!=registerSpace)continue;
            if(resolved!=UINT32_MAX)return false;
            resolved=index;
        }
        if(resolved==UINT32_MAX)return false;
        rootIndex=resolved;return true;
    }catch(...){return false;}
}
bool ResolveRootDescriptor(uintptr_t root,uint32_t rootIndex,uint64_t descriptorOffset,RootDescriptorEvidence& out)noexcept {
    out={};
    try {
        std::vector<uint8_t> blob;
        {
            std::lock_guard lock(detail::g_rootsig_mtx);
            const auto found=detail::g_rootsig_blobs.find(reinterpret_cast<void*>(root));
            if(found==detail::g_rootsig_blobs.end())return false;
            blob=found->second;
        }
        Microsoft::WRL::ComPtr<ID3D12VersionedRootSignatureDeserializer> deserializer;
        if(FAILED(D3D12CreateVersionedRootSignatureDeserializer(blob.data(),blob.size(),IID_PPV_ARGS(&deserializer))))return false;
        const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc{};
        if(FAILED(deserializer->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1,&desc))||!desc||rootIndex>=desc->Desc_1_1.NumParameters)return false;
        const auto& parameter=desc->Desc_1_1.pParameters[rootIndex];
        if(parameter.ParameterType!=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)return false;
        uint64_t nextOffset{};bool matched=false;RootDescriptorEvidence resolved;
        for(UINT index=0;index<parameter.DescriptorTable.NumDescriptorRanges;++index){
            const auto& range=parameter.DescriptorTable.pDescriptorRanges[index];
            const uint64_t offset=range.OffsetInDescriptorsFromTableStart==D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND?nextOffset:range.OffsetInDescriptorsFromTableStart;
            nextOffset=offset+range.NumDescriptors;
            if(descriptorOffset<offset||descriptorOffset-offset>=range.NumDescriptors)continue;
            const uint64_t shaderRegister=uint64_t(range.BaseShaderRegister)+descriptorOffset-offset;
            // Aliased table offsets can name more than one shader register.
            // Do not report an arbitrary first range as a unique resolution.
            if(shaderRegister>UINT32_MAX||matched)return false;
            resolved={parameter.ShaderVisibility,range.RangeType,index,range.BaseShaderRegister,uint32_t(shaderRegister),range.RegisterSpace,range.NumDescriptors,range.OffsetInDescriptorsFromTableStart,offset};
            matched=true;
        }
        if(matched){out=resolved;return true;}
    }catch(...){}
    return false;
}
}
#include "MenuPreview/EditorUiSurface.hpp"

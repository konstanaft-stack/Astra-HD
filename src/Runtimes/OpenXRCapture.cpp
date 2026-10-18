#include "Render/RenderProbeScope.hpp"
#include "MenuPreview/UiCameraCycle.hpp"
#include "MenuPreview/UiConsumer.hpp"
#include "Runtimes/EditorSceneCapturePolicy.hpp"
#include "Runtimes/EditorNativeUiPacketPolicy.hpp"
namespace cvr::detail {extern cvr::menu_preview::EditorSceneExecute g_orig_ExecuteCommandLists;}
#include "Utils/DebugGate.hpp"
#include "Framegen/AerCaptureDiagnostics.hpp"
#include "Framegen/CapturedHeadPolicy.hpp"
#include "Framegen/NativeFgPairedSource.hpp"
#include "Framegen/SourceFrameSystem.hpp"
#include "Render/CommandResources.hpp"
#include "Render/NativeColorSource.hpp"
#include "Render/GpuStageProfile.hpp"
#include "Framegen/ProducerTrace.hpp"
#include "Framegen/SubmittedImageProjection.hpp"
#include "Framegen/OwnSelectionWitness.hpp"
// openxr_capture.cpp - mono frame + depth capture and submit-resource setup.
// Split verbatim from openxr_manager.cpp (OpenXRManager methods). Shared module
// state/helpers via openxr_internal.h (inline).
#include <atomic>
#include <chrono>
extern "C" int CyberpunkVR_BindPoseToImage;
#include "Runtimes/OpenXRManager.hpp"
#include "Camera/ImagePoseIdentity.hpp"
#include "Camera/ImagePoseMath.hpp"
#include "Camera/CameraState.hpp"
#include "Overlay/ImGuiOverlay.hpp"   // OverlayRecordIntoTarget, the second-eye pass
#include "Runtimes/OpenXRInternal.hpp"
#include "Utils/XrMath.hpp"
#include "Utils/SharedSlots.hpp"
#include "Runtimes/RuntimeFovCorrection.hpp"
#include "Stereo/CetOverlayLayer.hpp"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <utility>
#include <chrono>
#include <thread>
#include <memory>
#include <algorithm>
#include <dxgi1_4.h>

namespace fgtrace=cvr::framegen;
static int32_t QueryNativeQueueForDiagnostic(void* object,void** result) noexcept {
    *result=nullptr;
    using Getter=int32_t(*)(void*,void**);
    const auto module=GetModuleHandleW(L"sl.interposer.dll");
    const auto getter=module?reinterpret_cast<Getter>(GetProcAddress(module,"slGetNativeInterface")):nullptr;
    if(!getter)return -1;
    __try {return getter(object,result);}
    __except(EXCEPTION_EXECUTE_HANDLER){*result=nullptr;return -2;}
}
static void ApplyProducerCaptureCompletion(fgtrace::ProducerTraceRecord& trace,const fgtrace::CaptureCompletion& completion) {
    trace.readyFence=uint64_t(reinterpret_cast<uintptr_t>(completion.fence.Get()));trace.readyValue=completion.fenceValue;
    trace.restingState=uint32_t(completion.colorState);trace.flags|=fgtrace::ResourceStateKnown;
    if(completion.HasScheduledCopySource())trace.flags|=fgtrace::ProducerFenceScheduled;
    trace.captureSubmissionQpc=completion.qpc.QuadPart;trace.captureQpcFrequency=completion.qpcFrequency;
    if(completion.qpc.QuadPart>0 && completion.qpcFrequency>0)trace.flags|=fgtrace::CaptureSubmissionQpcValid;
    // Publication evidence is not a pose/content clock and never enables
    // either time-valid bit. The completion point can still be pending on GPU.
}

// Right eye source, from the stereo module. "Fresh" returns null once the VRCAM view stops
// updating, which is what makes the fallback to mono automatic.
extern "C" ID3D12Resource* CyberpunkVR_GetVrcamEyeTextureFresh();
// The finished HUD surface, snapshotted by sync_stereo, plus the engine's own composite
// constants read out of its b6 buffer.
extern "C" ID3D12Resource* CyberpunkVR_GetHudTexture();
// The scanner's object outline. The second eye renders its OWN -- the writer is
// CRenderNode_RenderVisionElements, which dispatches for both views (it never showed up in the
// draw census because it does not draw). What VRCAM never gets is the chain that composites the
// layer, so sync_stereo snapshots it and it is blended in here. Copying MAIN's would be wrong on
// principle: an outline traces on-screen silhouettes, so MAIN's would sit at MAIN's parallax and
// read as double vision.
extern "C" ID3D12Resource* CyberpunkVR_GetVisionTexture();
extern "C" int CyberpunkVR_VisionToSecondEye;
extern "C" int CyberpunkVR_VisionDebug;         // 0 blend, 1 opaque replace, 2 additive
extern "C" int CyberpunkVR_VisionFit;           // 1 = pixel-exact rather than stretched
extern "C" float CyberpunkVR_VisionOffX;
extern "C" float CyberpunkVR_VisionOffY;
extern "C" unsigned long long CyberpunkVR_DebugVisionOverlays;
extern "C" ID3D12Resource* CyberpunkVR_GetHudBlurTexture();
extern "C" ID3D12Resource* CyberpunkVR_GetHudExposureBuffer();
extern "C" ID3D12Resource* CyberpunkVR_GetFrameConstantBuffer();
extern "C" ID3D12Resource* CyberpunkVR_GetHudConstantBuffer();
extern "C" void CyberpunkVR_NoteHudCompositeInputs(const void* hud, const void* blur,
                                                   const void* expo, const void* frameCb,
                                                   const void* hudCb);
extern "C" ID3D12Resource* CyberpunkVR_GetMainOutTexture();
extern "C" ID3D12Resource* CyberpunkVR_GetMainSceneTexture();
// Not extern "C" (it returns a C++ struct), so it keeps sync_stereo's namespace.
namespace cvr { ColorBlit::HudParams CyberpunkVR_GetHudParams(); }
extern "C" int CyberpunkVR_StereoSubmit;
// How far away the composited HUD should sit, in metres. 0 = leave it at infinity, which is what
// pasting it at the same pixel in both eyes amounts to. Live-tunable rather than a vrport.ini
// setting because the comfortable value is personal and worth sweeping in the headset.
//
// DEFAULT IS 0: NO SHIFT. It was 1.8 m, and at that distance the second eye's HUD is moved by
// IPD/1.8 radians -- which is correct as optics and wrong as a picture on this headset: markers and
// labels sit at visibly different places in the two eyes, which is what the disparity IS. The
// mechanism stays, gated on this value, because the finding that produced it is real and belongs to a
// different headset:
//
//   at zero disparity a Pimax tester reported "both images too far to the sides to converge" --
//   optical infinity means every icon splits by the full vergence angle when the eyes are focused an
//   arm's length away.
//
// So this is a per-headset preference with two defensible settings, not a bug with a fix. Set it to
// 1.8 (or sweep it) to get the finite-distance HUD back; leave it at 0 for identical placement in both
// eyes. The shift code below reads it every frame, so it can be moved live from the debugger.
extern "C" __declspec(dllexport) float CyberpunkVR_HudDistanceM = 0.0f;

// Our private right-eye target: the eye swapchain's exact format and size, so the submit can
// copy it with the same plain CopyResource it uses for MAIN. Both captures keep
// the shared copy/pixel/compute read mask until the producer obtains a write lease.
bool OpenXRManager::EnsureVrcamEyeTexture(uint32_t width, uint32_t height, DXGI_FORMAT format) {
    if (!m_d3dDevice || !width || !height || format == DXGI_FORMAT_UNKNOWN) return false;
    if (m_vrcamEyePool[0] && m_vrcamEyeW == width && m_vrcamEyeH == height &&
        m_vrcamEyeFmt == static_cast<uint32_t>(format)) {
        return true;
    }
    for (int i = 0; i < kVrcamEyeSlots; ++i) {
        if (m_vrcamEyePool[i]) { m_vrcamEyePool[i]->Release(); m_vrcamEyePool[i] = nullptr; }
        m_vrcamEyePoolSerial[i] = 0;
    }
    m_vrcamEyeSlot = 0;
    m_vrcamEyeSerial = 0;

    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;   // ColorBlit draws into it
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    for (int i = 0; i < kVrcamEyeSlots; ++i) {
        if (FAILED(m_d3dDevice->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &d, fgtrace::CaptureColorReadState, nullptr,
                IID_PPV_ARGS(&m_vrcamEyePool[i])))) {
            for (int k = 0; k <= i; ++k) {
                if (m_vrcamEyePool[k]) { m_vrcamEyePool[k]->Release(); m_vrcamEyePool[k] = nullptr; }
            }
            Log("OpenXRManager: Failed to create stereo capture texture eye=1 slot=%d %ux%u fmt=%u\n",
                i, width, height, static_cast<unsigned>(format));
            return false;
        }
        SetD3DNamef(m_vrcamEyePool[i], L"OpenXR_vrcam_eye_slot%d", i);
    }
    m_vrcamEyeW = width;
    m_vrcamEyeH = height;
    m_vrcamEyeFmt = static_cast<uint32_t>(format);
    Log("OpenXRManager: stereo capture pool ready, %d slots. eye=1 %ux%u fmt=%u\n",
        kVrcamEyeSlots, width, height, static_cast<unsigned>(format));
    return true;
}

bool OpenXRManager::EnsureMonoCaptureResource(const D3D12_RESOURCE_DESC& sourceDesc) {
    if (!m_d3dDevice || !m_d3dQueue) {
        return false;
    }

    const uint32_t width = static_cast<uint32_t>(sourceDesc.Width);
    const uint32_t height = sourceDesc.Height;
    const uint32_t format = static_cast<uint32_t>(sourceDesc.Format);
    if (width == 0 || height == 0 || format == 0) {
        return false;
    }

    if (!m_captureCmdAllocators[0]) {
        for (int i = 0; i < 3; ++i) {
            if (FAILED(m_d3dDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_captureCmdAllocators[i])))) {
                Log("OpenXRManager: Failed to create mono capture command allocator %d\n", i);
                return false;
            }
            SetD3DName(m_captureCmdAllocators[i], L"OpenXR_capture_allocator");
        }
    }
    if (!m_captureCmdLists[0]) {
        for (int i = 0; i < 3; ++i) {
            if (FAILED(m_d3dDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_captureCmdAllocators[i], nullptr, IID_PPV_ARGS(&m_captureCmdLists[i])))) {
                Log("OpenXRManager: Failed to create mono capture command list %d\n", i);
                return false;
            }
            SetD3DName(m_captureCmdLists[i], L"OpenXR_capture_command_list");
            m_captureCmdLists[i]->Close();
        }
    }
    if (!m_captureFence) {
        if (FAILED(m_d3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_captureFence)))) {
            Log("OpenXRManager: Failed to create mono capture fence\n");
            return false;
        }
        SetD3DName(m_captureFence, L"OpenXR_capture_fence");
    }
    if (!m_captureFenceEvent) {
        m_captureFenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!m_captureFenceEvent) {
            Log("OpenXRManager: Failed to create mono capture fence event\n");
            return false;
        }
    }

    const auto captureSettings=cvr::framegen::GetSettings();
    const bool pairedOwned=!captureSettings.aerV2 && captureSettings.enabled && captureSettings.backend==cvr::framegen::Backend::OwnNvidia;
    const bool separatedUi=cvr::menu_preview::UiStereoRequested()&&cvr::menu_preview::EditorUiSeparationEnabled();
    const auto desiredPoolCount=std::max(cvr::xr::EditorUiMinimumPool(separatedUi),
        std::max(pairedOwned?5u:(cvr::menu_preview::UiStereoRequested()?4u:3u),cvr::capture::MainPoolCapacity(captureSettings.aerV2,
        captureSettings.enabled && captureSettings.backend==cvr::framegen::Backend::OwnNvidia)));
    m_retiredMainPools.Collect(); // capture owner serializes all producer writes
    {
        std::lock_guard<std::mutex> lock(m_presentMutex);
        if (m_monoPool[0] &&
            (m_monoPoolCount == desiredPoolCount || ((pairedOwned||separatedUi) && m_monoPoolCount >= desiredPoolCount)) &&
            m_monoCapturedFrame.width == width &&
            m_monoCapturedFrame.height == height &&
            m_monoCapturedFrame.format == format) {
            return true;
        }

        // Keep the old allocation published while preparing the replacement.
        // Failure to allocate must not leave a half-built pool in use.
    }

    if(cvr::menu_preview::UiStereoRequested())ClearMenuUiPair(); // resize invalidates publications; consumer leases retire normally
    D3D12_HEAP_PROPERTIES heapProps{};
    if(!m_retiredMainPools.CanRetire(m_mainPoolProducer))return false;
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    ID3D12Resource* created[cvr::capture::MainPoolMaximum] = {};
    for (uint32_t i = 0; i < desiredPoolCount; ++i) {
        if (FAILED(fgtrace::CreateMainCaptureColor(m_d3dDevice,heapProps,sourceDesc,&created[i]))) {
            Log("OpenXRManager: Failed to create mono captured texture %d\n", i);
            for (uint32_t k = 0; k < i; ++k) created[k]->Release();
            return false;
        }
        SetD3DNamef(created[i], L"OpenXR_mono_snapshot_color%d", i);
    }

    {
        std::lock_guard<std::mutex> lock(m_presentMutex);
        // Outstanding readers retain their own COM + pool leases. The pool
        // relinquishes only its ownership; no reader is forcibly unpinned.
        if(!m_retiredMainPools.Retire(std::span(m_monoPool).first(m_monoPoolCount),m_mainPoolProducer)) {
            for(auto* resource:created)if(resource)resource->Release();
            return false;
        }
        for(auto*& resource:m_monoPool) {if(resource)resource->Release();resource=nullptr;}
        for (uint32_t i = 0; i < desiredPoolCount; ++i) m_monoPool[i] = created[i];
        m_monoPoolCount=desiredPoolCount;
        m_mainPoolProducer={};
        ++m_captureResourceEpoch;
        std::array<uintptr_t,cvr::capture::MainPoolMaximum> catalogueResources{};
        for(uint32_t i=0;i<m_monoPoolCount;++i)catalogueResources[i]=reinterpret_cast<uintptr_t>(m_monoPool[i]);
        m_mainCaptureCatalogue.Reset(m_captureResourceEpoch,std::span(catalogueResources).first(m_monoPoolCount));
        m_monoPoolSlot = 0;
        m_monoCapturedFrame.texture = m_monoPool[0];
        m_monoCapturedFrame.width = width;
        m_monoCapturedFrame.height = height;
        m_monoCapturedFrame.format = format;
        m_monoCapturedFrame.serial = 0;
        m_monoCapturedFrame.colorCompletion = {};
        m_monoCapturedFrame.nativeStamp = {};
        m_monoCapturedFrame.hasView[0] = false;
        m_monoCapturedFrame.hasView[1] = false;
    }

    Log("OpenXRManager: Mono snapshot resource ready. size=%ux%u format=%u\n", width, height, format);
    return true;
}

// [DEPTH] Accessors implemented in swapchain_hooks.cpp — the game's pinned
// scene depth resource and its CURRENT (observed) D3D12 resource state.
extern "C" ID3D12Resource* OmoAcquireSceneDepthResource(); // returns one owned reference
extern "C" unsigned int OmoGetSceneDepthState();
extern "C" unsigned int OmoGetSceneDepthWidth();
extern "C" unsigned int OmoGetSceneDepthHeight();
extern "C" unsigned int OmoGetSceneDepthFormat();
extern "C" ID3D12CommandQueue* OmoGetSceneDepthWriterQueue(); // game's depth-writer queue (safe mono depth capture)

bool OpenXRManager::EnsureDepthSnapshot(ID3D12Resource* gameDepth) {
    std::lock_guard<std::mutex> snapshotLock(m_presentMutex);
    if (!gameDepth || !m_d3dDevice) {
        return false;
    }
    // Depth submit is OFF by default (xr_depth_submit=0). Copying the game's LIVE
    // scene-depth resource on our capture queue races the game's own queue (which is
    // simultaneously writing DepthPrepass/GBuffer and reallocating render targets on
    // load/spawn). That cross-queue access caused GPU device-hung (0x887a0006) under
    // VDXR, where the scene depth is an R32-family format the snapshot path accepts.
    // depth gave no confirmed benefit (the left-eye fix was the alternate-eye pose-pair
    // lock, not depth), so keep it gated unless explicitly re-enabled for experiments.
    if (GetDepthSubmit() == 0) {
        if (m_depthLayerSupported) {
            Log("OpenXRManager: [DEPTH] depth submit disabled (xr_depth_submit=0)\n");
        }
        m_depthLayerSupported = false;
        m_depthSwapchainFormat = 0;
        m_depthSnapshotSerial = 0;
        return false;
    }
    const D3D12_RESOURCE_DESC desc = gameDepth->GetDesc();
    // Accept both R32 (32bpp) and R32G8X24 (64bpp) source families. The 64bpp
    // path uses DepthResolve shader to extract plane 0 (the float depth) into
    // a 32bpp D32_FLOAT snapshot which is bit-compatible with the standard
    // depth swapchain — no more DEVICE_HUNG, no more sceneindentation depth=0
    // in submit logs. Old comment about "TYPELESS snapshot required" is
    // obsolete: now the snapshot is typed D32_FLOAT, populated by shader.
    const bool acceptable32 =
        desc.Format == DXGI_FORMAT_R32_TYPELESS ||
        desc.Format == DXGI_FORMAT_D32_FLOAT ||
        desc.Format == DXGI_FORMAT_R32_FLOAT;
    const bool acceptable64 =
        desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS ||
        desc.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
        desc.Format == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS ||
        desc.Format == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
    if (!acceptable32 && !acceptable64) {
        if (m_depthLayerSupported) {
            Log("OpenXRManager: [DEPTH] disabling depth layer for unsupported source format=%u\n",
                static_cast<unsigned>(desc.Format));
        }
        m_depthLayerSupported = false;
        m_depthSwapchainFormat = 0;
        m_depthSnapshotSerial = 0;
        return false;
    }
    // Snapshot always D32_FLOAT 32bpp now. For R32-family sources the capture
    // path uses CopyTextureRegion (bit-compat). For 64bpp sources the capture
    // path uses DepthResolve shader (plane 0 extract). Either way, downstream
    // depth swapchain copy works with a single typed format.
    const DXGI_FORMAT snapshotFormat = DXGI_FORMAT_D32_FLOAT;
    if (m_depthSnapshot) {
        const D3D12_RESOURCE_DESC cur = m_depthSnapshot->GetDesc();
        if (cur.Width == desc.Width && cur.Height == desc.Height && cur.Format == snapshotFormat) {
            return true;
        }
        m_depthSnapshot->Release();
        m_depthSnapshot = nullptr;
        m_depthSnapshotSerial = 0;
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC sd = desc;
    sd.Format = snapshotFormat;
    sd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    // Typed depth resources with ALLOW_DEPTH_STENCIL require a clear value.
    D3D12_CLEAR_VALUE clearVal{};
    clearVal.Format = snapshotFormat;
    clearVal.DepthStencil.Depth = 1.0f;
    clearVal.DepthStencil.Stencil = 0;
    const HRESULT hr = m_d3dDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &sd,
        D3D12_RESOURCE_STATE_COPY_DEST, &clearVal, IID_PPV_ARGS(&m_depthSnapshot));
    if (FAILED(hr)) {
        Log("OpenXRManager: [DEPTH] CreateCommittedResource(depthSnapshot) failed hr=0x%08X\n", hr);
        m_depthSnapshot = nullptr;
        return false;
    }
    m_depthSnapshotW = static_cast<uint32_t>(desc.Width);
    m_depthSnapshotH = desc.Height;
    m_depthSnapshotSerial = 0;
    SetD3DName(m_depthSnapshot, L"OpenXR_scene_depth_snapshot");
    Log("OpenXRManager: [DEPTH] snapshot created %llux%u srcFmt=%u snapFmt=%u\n",
        static_cast<unsigned long long>(desc.Width), desc.Height,
        static_cast<unsigned>(desc.Format), static_cast<unsigned>(snapshotFormat));

    return true;
}

void OpenXRManager::CaptureSceneDepthInline(ID3D12GraphicsCommandList* list,
                                            ID3D12Resource* gameDepth,
                                            unsigned int stateAfter) {
    if (!list || !gameDepth || !m_d3dDevice) return;
    // The consumer rejects menu/loading depth too. Do not create/copy transient
    // startup textures that will never be submitted to the headset.
    if (!GetMonoDepthCapture() || !GetDepthSubmit() || GetMenuMode() || GetMenuRectMode() ||
        !m_sessionRunning.load(std::memory_order_relaxed) || m_stopFrameThread.load(std::memory_order_relaxed) ||
        !m_depthStageCaptureAllowed.load(std::memory_order_acquire)) return;
    // The engine can make the depth readable more than once per frame; one copy is enough.
    uint64_t frame = m_presentCount.load(std::memory_order_relaxed);
    if (m_depthStageFrame.load(std::memory_order_acquire) == frame) return;
    std::unique_lock<std::mutex> lock(m_depthStageMutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    frame = m_presentCount.load(std::memory_order_relaxed);
    if (m_depthStageFrame.load(std::memory_order_relaxed) == frame) return;

    D3D12_RESOURCE_DESC srcDesc = gameDepth->GetDesc();
    if (srcDesc.Width == 0 || srcDesc.Height == 0) return;

    // Copy the DEPTH PLANE ONLY, not the whole planar surface.
    //
    // CP2077's scene depth is R32G8X24_TYPELESS: 64 bits per pixel, of which 32 are stencil
    // padding we have no use for. Copying it whole cost ~48 MB per frame at 2444x2444, about
    // 3 GB/s of extra bandwidth on the engine's own command list -- enough to show up as
    // occasional hitches. Plane 0 of that format is the R32 depth, so a 32bpp stage halves
    // the traffic, and it also removes the shader resolve later: the stage is then already
    // in the layout the depth swapchain wants.
    const DXGI_FORMAT srcFmt = srcDesc.Format;
    const bool planar64 =
        srcFmt == DXGI_FORMAT_R32G8X24_TYPELESS ||
        srcFmt == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
        srcFmt == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS ||
        srcFmt == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
    const DXGI_FORMAT stageFmt = planar64 ? DXGI_FORMAT_R32_TYPELESS : srcFmt;

    if (!m_depthStage || m_depthStageW != srcDesc.Width || m_depthStageH != srcDesc.Height ||
        m_depthStageFmt != static_cast<uint32_t>(stageFmt)) {
        if (m_depthStage) { m_depthStage->Release(); m_depthStage = nullptr; }
        D3D12_RESOURCE_DESC d = srcDesc;
        d.Format = stageFmt;
        d.Flags = D3D12_RESOURCE_FLAG_NONE;   // plain texture: copy target + copy source
        d.MipLevels = 1;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(m_d3dDevice->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&m_depthStage)))) {
            m_depthStage = nullptr;
            return;
        }
        m_depthStageW = static_cast<uint32_t>(srcDesc.Width);
        m_depthStageH = srcDesc.Height;
        m_depthStageFmt = static_cast<uint32_t>(stageFmt);
        m_depthStageSerial = 0;
        SetD3DNamef(m_depthStage, L"OpenXR_depth_stage");
    }
    if (!m_depthStage) return;
    // A size change can release the owner's old stage immediately, but every
    // recorded copy retains its exact source/destination until the GPU is done.
    if (!cvr::gpu::KeepCommandResources(list, {gameDepth, m_depthStage})) return;
    // Publish before ResourceBarrier re-enters the depth hook.
    m_depthStageFrame.store(frame, std::memory_order_release);

    const auto before = static_cast<D3D12_RESOURCE_STATES>(stateAfter);
    D3D12_RESOURCE_BARRIER pre[2] = {};
    UINT n = 0;
    // The source is shader-readable right now; a copy needs COPY_SOURCE. Both transitions
    // are restored below, so the engine finds the buffer exactly as it left it.
    pre[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    pre[n].Transition.pResource = gameDepth;
    pre[n].Transition.StateBefore = before;
    pre[n].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    pre[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ++n;
    if (m_depthStageSerial != 0) {          // first use it is already in COPY_DEST
        pre[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        pre[n].Transition.pResource = m_depthStage;
        pre[n].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        pre[n].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        pre[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ++n;
    }
    list->ResourceBarrier(n, pre);

    if (planar64) {
        // Subresource 0 of a planar depth-stencil is the depth plane; the stencil plane is
        // left where it is. Half the bytes of a full-surface copy.
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = m_depthStage;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = gameDepth;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    } else {
        list->CopyResource(m_depthStage, gameDepth);
    }

    D3D12_RESOURCE_BARRIER post[2] = {};
    post[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    post[0].Transition.pResource = gameDepth;
    post[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    post[0].Transition.StateAfter = before;                  // exactly as the engine had it
    post[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    post[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    post[1].Transition.pResource = m_depthStage;
    post[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    // COPY_SOURCE, because the stage is now plain 32bpp depth: the later step is a straight
    // copy into the depth swapchain, not a shader resolve.
    post[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    post[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(2, post);

    // The frame being built is the one the NEXT present shows, so tag it accordingly.
    m_depthStageSerial = frame + 1;

    // Bind THIS frame's head pose here too, for the same reason and with the same stamp.
    //
    // The camera injection for this frame has already run (it happens before the geometry
    // pass, and this barrier is after it), so the newest entry in the injection ring is
    // unambiguously the pose the engine is drawing with right now. No counting backwards, no
    // lag to tune: the image, its depth and its pose all get one identity, which is what
    // keeps them together through the engine's render-ahead the way RealVR's per-slot pose
    // does.
    {
        // Frame-pose publication moved to PushRenderHeadPose: tying it to the depth
        // barrier meant ~25% of frames had no exact pose and fell back to an estimate.
    }
}

bool OpenXRManager::RecordDepthCapture(ID3D12GraphicsCommandList* cmdList,
                                       ID3D12Resource* gameDepth,
                                       D3D12_RESOURCE_STATES gameDepthState,
                                       bool transitionGameDepth) {
    if (!cmdList || !gameDepth || !m_depthSnapshot) return false;
    if (!cvr::gpu::KeepCommandResources(cmdList, {gameDepth, m_depthSnapshot})) return false;
    const DXGI_FORMAT srcFmt = gameDepth->GetDesc().Format;
    const bool is64bpp =
        srcFmt == DXGI_FORMAT_R32G8X24_TYPELESS ||
        srcFmt == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
        srcFmt == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS ||
        srcFmt == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;

    if (!is64bpp) {
        // 32bpp path needs the source in COPY_SOURCE. When we're not allowed to
        // transition the game resource, only proceed if it is already there.
        if (!transitionGameDepth && gameDepthState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            return false;
        }
        // 32bpp path: simple CopyTextureRegion (bit-compat between R32/D32).
        D3D12_RESOURCE_BARRIER pre[2] = {};
        UINT preCount = 0;
        if (transitionGameDepth && gameDepthState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            pre[preCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            pre[preCount].Transition.pResource = gameDepth;
            pre[preCount].Transition.StateBefore = gameDepthState;
            pre[preCount].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            pre[preCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ++preCount;
        }
        if (m_depthSnapshotSerial != 0) {
            pre[preCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            pre[preCount].Transition.pResource = m_depthSnapshot;
            pre[preCount].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            pre[preCount].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            pre[preCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ++preCount;
        }
        if (preCount > 0) cmdList->ResourceBarrier(preCount, pre);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = m_depthSnapshot;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = gameDepth;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER post[2] = {};
        UINT postCount = 0;
        post[postCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        post[postCount].Transition.pResource = m_depthSnapshot;
        post[postCount].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        post[postCount].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        post[postCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ++postCount;
        if (transitionGameDepth && gameDepthState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            post[postCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            post[postCount].Transition.pResource = gameDepth;
            post[postCount].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            post[postCount].Transition.StateAfter = gameDepthState;
            post[postCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ++postCount;
        }
        cmdList->ResourceBarrier(postCount, post);
        return true;
    }

    // 64bpp path: shader resolve plane 0 → D32_FLOAT DSV.
    if (!m_depthResolve) m_depthResolve = std::make_unique<DepthResolve>();
    if (!m_depthResolve->EnsureInitialized(m_d3dDevice, DXGI_FORMAT_D32_FLOAT,
            m_depthSnapshotW, m_depthSnapshotH)) {
        return false;
    }

    D3D12_RESOURCE_BARRIER pre[2] = {};
    UINT preCount = 0;
    if (transitionGameDepth && gameDepthState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
        pre[preCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        pre[preCount].Transition.pResource = gameDepth;
        pre[preCount].Transition.StateBefore = gameDepthState;
        pre[preCount].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        pre[preCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ++preCount;
    }
    // Snapshot was last left in COPY_SOURCE if we've written before; first
    // time it's in COPY_DEST (created with that state). Either way go to
    // DEPTH_WRITE for the resolve draw.
    pre[preCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    pre[preCount].Transition.pResource = m_depthSnapshot;
    pre[preCount].Transition.StateBefore = (m_depthSnapshotSerial != 0)
        ? D3D12_RESOURCE_STATE_COPY_SOURCE
        : D3D12_RESOURCE_STATE_COPY_DEST;
    pre[preCount].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    pre[preCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ++preCount;
    cmdList->ResourceBarrier(preCount, pre);

    const bool ok = m_depthResolve->RecordResolve(cmdList, gameDepth, m_depthSnapshot);

    D3D12_RESOURCE_BARRIER post[2] = {};
    UINT postCount = 0;
    post[postCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    post[postCount].Transition.pResource = m_depthSnapshot;
    post[postCount].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    post[postCount].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    post[postCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ++postCount;
    if (transitionGameDepth && gameDepthState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
        post[postCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        post[postCount].Transition.pResource = gameDepth;
        post[postCount].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        post[postCount].Transition.StateAfter = gameDepthState;
        post[postCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ++postCount;
    }
    cmdList->ResourceBarrier(postCount, post);
    return ok;
}

bool OpenXRManager::CaptureMonoDepthOnWriterQueue(uint64_t serial) {
    if (GetMonoDepthCapture() == 0) return false;
    Microsoft::WRL::ComPtr<ID3D12Resource> depthLease;
    depthLease.Attach(OmoAcquireSceneDepthResource());
    ID3D12Resource* gameDepth = depthLease.Get();
    ID3D12CommandQueue* writerQueue = OmoGetSceneDepthWriterQueue();
    const D3D12_RESOURCE_STATES gameDepthState = static_cast<D3D12_RESOURCE_STATES>(OmoGetSceneDepthState());
    // No writer queue discovered yet (or no explicit depth state observed) -> skip this
    // frame. This is a soft skip (no depth submitted), never a hang.
    if (!m_d3dDevice || !gameDepth || !writerQueue || OmoGetSceneDepthState() == 0) return false;
    if (!EnsureDepthSnapshot(gameDepth)) return false;

    // Lazily create the dedicated resolve list + fence (DIRECT; the writer queue is a
    // graphics queue in CP2077).
    if (!m_depthWriterAlloc) {
        if (FAILED(m_d3dDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_depthWriterAlloc)))) return false;
        SetD3DName(m_depthWriterAlloc, L"OpenXR_depth_writer_alloc");
    }
    if (!m_depthWriterList) {
        if (FAILED(m_d3dDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_depthWriterAlloc, nullptr, IID_PPV_ARGS(&m_depthWriterList)))) return false;
        SetD3DName(m_depthWriterList, L"OpenXR_depth_writer_list");
        m_depthWriterList->Close();
    }
    if (!m_depthWriterFence) {
        if (FAILED(m_d3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_depthWriterFence)))) return false;
        SetD3DName(m_depthWriterFence, L"OpenXR_depth_writer_fence");
    }
    // Single-slot: if the previous resolve hasn't finished, skip (don't stomp the list
    // or m_depthSnapshot while the GPU still reads/writes it).
    if (m_depthWriterFenceValue != 0 && m_depthWriterFence->GetCompletedValue() < m_depthWriterFenceValue) {
        return false;
    }
    if (FAILED(m_depthWriterAlloc->Reset()) || FAILED(m_depthWriterList->Reset(m_depthWriterAlloc, nullptr))) {
        return false;
    }
    const bool ok = RecordDepthCapture(m_depthWriterList, gameDepth, gameDepthState);
    m_depthWriterList->Close();
    if (!ok) return false;

    // Execute on the GAME's depth-writer queue: naturally ordered AFTER the depth write
    // that just ran on the same queue (FIFO), so no cross-queue Wait and no hang.
    ID3D12CommandList* lists[] = { m_depthWriterList };
    writerQueue->ExecuteCommandLists(1, lists);
    const UINT64 fv = ++m_depthWriterFenceValue;
    writerQueue->Signal(m_depthWriterFence, fv);
    m_depthSnapshotWriterFence = fv;  // submit path waits on this before reading m_depthSnapshot
    return true;
}

// 1 = while a menu is open, both eyes get the backbuffer (the menu) instead of pairing it with
// VRCAM_s stale view of the world. 0 = the previous behaviour, for A/B.
extern "C" __declspec(dllexport) int CyberpunkVR_MonoMenu = 1;


// ---- WHY A PRESENT DID NOT PRODUCE A CAPTURE -------------------------------------------------
//
// Measured because the frame accounting proved this path is where the jumps come from, and could not
// say which of the five early-outs below causes them.
//
// The evidence: in GAMEPLAY windows (presents 40-70/s, so no menus or loading), a submit carrying an
// image 30-53 ms old happens once every ~3.9 seconds, and in those windows the PRESENT RATE IS
// UNCHANGED -- 50.3/s against 51.4/s in clean windows. The game kept delivering frames; the capture
// did not publish one for two or three display periods. So the cause is here, not in the game.
//
// The fence wait is the prime suspect and is timed rather than just counted: it runs ON THE GAME'S
// PRESENT THREAD, so every millisecond it blocks is a millisecond the game is not presenting -- which
// would explain both the stale submit AND the 1.1/s present-rate difference at once.
extern "C" __declspec(dllexport) std::atomic<unsigned long long> CyberpunkVR_DebugCapSkipNoView   = 0;
extern "C" __declspec(dllexport) cvr::framegen::AerCaptureDiagnostics CyberpunkVR_AerCaptureDebug{};
extern "C" __declspec(dllexport) std::atomic<unsigned long long> CyberpunkVR_DebugCapSkipNoRes    = 0;
extern "C" __declspec(dllexport) std::atomic<unsigned long long> CyberpunkVR_DebugCapSkipNoSlot   = 0;
extern "C" __declspec(dllexport) std::atomic<unsigned long long> CyberpunkVR_DebugCapSkipFence    = 0;
extern "C" __declspec(dllexport) std::atomic<unsigned long long> CyberpunkVR_DebugCapSkipReset    = 0;
// Defined in the stereo module (src/Stereo/Capture.cpp): the second-eye content age it already
// computes for its own staleness gate. Sampled below rather than recomputed.
extern "C" __declspec(dllexport) extern uint32_t CyberpunkVR_DebugVrcamEyeAgeMs;
extern "C" __declspec(dllexport) extern uint32_t CyberpunkVR_DebugVrcamEyeAgeUs;

extern "C" __declspec(dllexport) std::atomic<unsigned long long> CyberpunkVR_DebugCapOk           = 0;
// The second eye's CONTENT age -- see the sampling site for why this is not the same thing as [xreye].
extern "C" __declspec(dllexport) std::atomic<unsigned long long> CyberpunkVR_DebugEyeAgeCount     = 0;
extern "C" __declspec(dllexport) std::atomic<unsigned long long> CyberpunkVR_DebugEyeAgeSumMs     = 0;
extern "C" __declspec(dllexport) std::atomic<unsigned long long> CyberpunkVR_DebugEyeAgeMaxMs     = 0;
extern "C" __declspec(dllexport) std::atomic<unsigned long long> CyberpunkVR_DebugEyeAgeNever     = 0;
extern "C" __declspec(dllexport) std::atomic<unsigned long long> CyberpunkVR_DebugEyeAgeBuckets[4] = {};
// The bounded fence wait: how often it is entered at all, and how long it actually blocked.
extern "C" __declspec(dllexport) std::atomic<unsigned long long> CyberpunkVR_DebugCapFenceWaits   = 0;
extern "C" __declspec(dllexport) std::atomic<unsigned long long> CyberpunkVR_DebugCapFenceUsSum   = 0;
extern "C" __declspec(dllexport) std::atomic<unsigned long long> CyberpunkVR_DebugCapFenceUsMax   = 0;

ID3D12Resource* OpenXRManager::AcquireCapturedVrcamLocked(uint64_t serial,uint32_t maxReuse,
        XrPosef* pose,XrFovf* fov,uint64_t* poseId,bool* reused) {
    int best=-1;
    uint64_t bestSerial=0,newestStale=0;
    const auto traceSelection=[&](fgtrace::ProducerReason reason,uint64_t source,uint64_t sourcePose,bool reserved) {
        fgtrace::ProducerTraceRecord trace;trace.stage=fgtrace::ProducerStage::VrcamReadSelection;trace.reason=reason;
        trace.engineView=fgtrace::EngineView::Vrcam;trace.flags=fgtrace::EngineViewKnown;
        trace.requestedSourceId=serial;if(serial)trace.flags|=fgtrace::RequestedSourceIdKnown;
        trace.sourceId=source;if(source)trace.flags|=fgtrace::SourceIdKnown;
        trace.poseId=sourcePose;if(sourcePose)trace.flags|=fgtrace::PoseIdKnown;
        if(reserved)trace.flags|=fgtrace::ColorReadReserved;
        if(reason==fgtrace::ProducerReason::VrcamSlotReused)trace.flags|=fgtrace::ColorSlotReused;
        if(reason==fgtrace::ProducerReason::VrcamSlotTooOld)trace.flags|=fgtrace::ColorSlotStaleRejected;
        // Only the matching publication owns this completion tuple. An older
        // reused slot must not borrow the current capture's fence or QPC.
        if(reserved && source==m_monoCapturedFrame.serial)
            ApplyProducerCaptureCompletion(trace,m_monoCapturedFrame.colorCompletion);
        // This accessor selects an engine VRCAM resource. Its caller owns the
        // output-eye mapping, so LogicalEyeKnown remains false here.
        fgtrace::RecordProducerTrace(trace);
    };
    for(int i=0;i<kVrcamEyeSlots;++i) {
        const auto sourceSerial=m_vrcamEyePoolSerial[i];
        auto* source=m_vrcamEyePool[i];
        if(!source || !sourceSerial || sourceSerial>serial || !m_vrcamEyeLabels[i].valid ||
           m_captureLeases.Writing(reinterpret_cast<uintptr_t>(source)))continue;
        if(sourceSerial!=serial && (!maxReuse || serial-sourceSerial>maxReuse)) {
            newestStale=std::max(newestStale,sourceSerial);continue;
        }
        if(sourceSerial>bestSerial) { best=i;bestSerial=sourceSerial; }
    }
    if(best<0) {
        traceSelection(newestStale ? fgtrace::ProducerReason::VrcamSlotTooOld : fgtrace::ProducerReason::VrcamSlotUnavailable,
            newestStale,0,false);return nullptr;
    }
    auto* resource=m_vrcamEyePool[best];
    if(!m_captureLeases.Pin(reinterpret_cast<uintptr_t>(resource))) {
        traceSelection(fgtrace::ProducerReason::VrcamSlotUnavailable,bestSerial,0,false);return nullptr;
    }
    resource->AddRef();
    const auto& label=m_vrcamEyeLabels[best];
    *pose=label.pose;*fov=label.fov;*poseId=label.poseId;*reused=bestSerial!=serial;
    traceSelection(*reused ? fgtrace::ProducerReason::VrcamSlotReused : fgtrace::ProducerReason::VrcamSlotFresh,
        bestSerial,label.poseId,true);
    return resource;
}

void OpenXRManager::ReleaseCapturedRead(ID3D12Resource* resource,uint64_t fence) {
    if(!resource)return;
    std::vector<ID3D12Resource*> release;
    {
        std::lock_guard lock(m_presentMutex);
        m_submitFenceRetirement.PollCompleted();
        const auto completion=m_submitFenceRetirement.ResolveRead(fence);
        if(!completion.Complete())m_retiredCaptureReads.push_back({resource,completion});
        else {
            m_captureLeases.Unpin(reinterpret_cast<uintptr_t>(resource));
            release.push_back(resource);
        }
        CollectCapturedReadsLocked(release);
    }
    for(auto* item:release)item->Release();
    m_mainCaptureAvailable.notify_all();
}

std::vector<cvr::framegen::Frame> OpenXRManager::CollectReadyMainCaptures(
    const cvr::framegen::MainEyeSourceReceipt& expectedScope) {
    using Catalogue=cvr::capture::MainCaptureCatalogue<cvr::framegen::Frame,cvr::capture::MainPoolMaximum>;
    std::vector<cvr::framegen::Frame> out;
    if(!expectedScope)return out;
    out.reserve(cvr::capture::MainPoolMaximum);
    const auto ready=[&](const Catalogue::Entry& entry) {
        const auto& key=entry.key;const auto& frame=entry.payload;
        const auto eye=uint32_t(entry.receipt.physicalEye);
        if(key.slot>=m_monoPoolCount || key.poolEpoch!=m_captureResourceEpoch ||
           key.resource!=reinterpret_cast<uintptr_t>(m_monoPool[key.slot]) || eye>1 ||
           frame.color[eye]!=m_monoPool[key.slot] || frame.color[eye^1] ||
           frame.nativeStamp.resourceEpoch!=m_captureResourceEpoch ||
           frame.nativeStamp.eyes[eye].mainEyeSource!=entry.receipt ||
           frame.identity.serial!=entry.serial || !frame.projection[eye] ||
           frame.lease || frame.inputs[0] || frame.inputs[1] ||
           m_captureLeases.Writing(key.resource) || !frame.colorCompletion.HasScheduledCopySource())return false;
        const auto done=frame.colorCompletion.fence->GetCompletedValue();
        return done!=UINT64_MAX && done>=frame.colorCompletion.fenceValue;
    };
    Catalogue::Keys keys;
    {std::lock_guard lock(m_presentMutex);keys=m_mainCaptureCatalogue.ReadyKeys(expectedScope,ready);}
    // Allocate owners outside the publication lock. Their callbacks also take
    // this lock, so conversion/rejection/destruction must never run inside it.
    std::array<std::shared_ptr<cvr::framegen::ReadLease>,cvr::capture::MainPoolMaximum> leases;
    for(size_t i=0;i<keys.size;++i){
        leases[i]=std::make_shared<cvr::framegen::ReadLease>();leases[i]->owner=this;
        leases[i]->release=[](void* owner,ID3D12Resource* resource,uint64_t fence){
            static_cast<OpenXRManager*>(owner)->ReleaseCapturedRead(resource,fence);
        };
    }
    {
        std::lock_guard lock(m_presentMutex);
        for(size_t i=0;i<keys.size;++i){
            const auto* entry=m_mainCaptureCatalogue.Find(keys.keys[i]);
            if(!entry || !cvr::capture::SameMainCaptureScope(entry->receipt,expectedScope) || !ready(*entry))continue;
            const auto eye=uint32_t(entry->receipt.physicalEye);
            if(!m_captureLeases.Pin(entry->key.resource))continue;
            auto* resource=entry->payload.color[eye];resource->AddRef();
            leases[i]->resources[eye]=resource;
            out.push_back(entry->payload);out.back().lease=std::move(leases[i]);
            // The lease now protects the immutable version. Do not rediscover
            // the same queued publication on every compositor tick.
            m_mainCaptureCatalogue.Claim(entry->key);
        }
    }
    // The live source owner can revoke a scope independently of the color
    // mutex. Recheck after pin; a rejected Frame releases its own lease here.
    out.erase(std::remove_if(out.begin(),out.end(),[](const auto& frame){
        const auto eye=frame.color[0]?0u:1u;
        return !cvr::camera::ValidateMainEyeSourceReceipt(frame.nativeStamp.eyes[eye].mainEyeSource);
    }),out.end());
    return out;
}

void OpenXRManager::CollectCapturedReadsLocked(std::vector<ID3D12Resource*>& release) {
    for(auto it=m_retiredCaptureReads.begin();it!=m_retiredCaptureReads.end();) {
        if(it->completion.Complete()) {
            m_captureLeases.Unpin(reinterpret_cast<uintptr_t>(it->resource));
            release.push_back(it->resource);it=m_retiredCaptureReads.erase(it);
        } else ++it;
    }
}

bool OpenXRManager::PollSubmitResources(bool requireComplete) {
    std::vector<ID3D12Resource*> release;bool ready{},becameUnknown{};
    {
        std::lock_guard lock(m_presentMutex);
        const bool wasUnknown=m_submitFenceRetirement.Unknown();
        m_submitFenceRetirement.PollCompleted();
        becameUnknown=!wasUnknown && m_submitFenceRetirement.Unknown();
        ready=!m_submitFenceRetirement.Unknown() &&
            (!requireComplete || m_submitFenceRetirement.CanRetire(m_fenceValue));
        CollectCapturedReadsLocked(release);
    }
    for(auto* item:release)item->Release();
    if(becameUnknown)Log("OpenXRManager: app XR fence completion invalid/device removed; disabling submission and retaining resources.\n");
    return ready;
}

void OpenXRManager::RecordSubmitExecuted(bool signalSucceeded) {
    bool becameUnknown{};
    {
        std::lock_guard lock(m_presentMutex);
        const bool wasUnknown=m_submitFenceRetirement.Unknown();
        m_submitFenceRetirement.Executed(m_fence,m_fenceValue,signalSucceeded);
        becameUnknown=!wasUnknown && m_submitFenceRetirement.Unknown();
    }
    if(becameUnknown)Log("OpenXRManager: app XR Signal completion unknown; disabling further submission and retaining resources for this owner.\n");
}

bool OpenXRManager::CreateSubmitFence() {
    std::lock_guard lock(m_presentMutex);
    if(m_fence || !m_submitFenceRetirement.CanRetire(m_fenceValue))return false;
    if(FAILED(m_d3dDevice->CreateFence(m_submitFenceRetirement.CompletedThrough(),
        D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&m_fence)))) {
        Log("OpenXRManager: Failed to create submit fence\n");return false;
    }
    return true;
}

namespace cvr::menu_preview {void AcknowledgeUiCapture(const UiFrameReceipt&)noexcept;}
namespace {
cvr::menu_preview::UiPairScope MenuUiScope(const cvr::menu_preview::Metadata& m,uint64_t pool){return {m.screenEpoch,m.setupID,m.controllerIdentity,m.componentIdentity,m.virtualCameraNameHash,pool};}
bool MenuEditorUiValid(const cvr::menu_preview::UiFrameReceipt& receipt){
    if(cvr::menu_preview::EditorUiSeparationEnabled()&&receipt.scope.screen==2&&!receipt.editorUiRequired)return false;
    if(!receipt.editorUiRequired)return true;
    const auto& ui=receipt.editorUi;
    return cvr::menu_preview::EditorUiSeparationEnabled()&&ui&&
        cvr::xr::EditorUiAttachmentMatches(*ui,receipt)&&cvr::xr::EditorUiSize(ui->renderAspect)&&
        ui->image&&ui->image->canvas&&ui->image->retainedOwner&&ui->image->serial&&
        ui->image->producerFence&&ui->image->producerValue&&ui->image->producerValue!=UINT64_MAX&&
        ui->image->producerFence->GetCompletedValue()!=UINT64_MAX;
}
void NoteEditorNativeUiProof(const cvr::menu_preview::UiFrameReceipt* receipt,const cvr::menu_preview::Metadata& common,
    uint64_t controls,uint64_t origin,uint64_t binding,uint64_t now)noexcept{
    using namespace cvr::menu_preview;
    try{static std::mutex mutex;static Metadata previous;static uint64_t previousBinding{},checks{};uint64_t count{};
        {std::lock_guard lock(mutex);if(!SameEditorApplicationScope(previous,common)||previousBinding!=binding){previous=common;previousBinding=binding;checks=0;}count=++checks;}
        if(count>4&&count!=128&&count!=1024)return;
        const auto* app=receipt?receipt->application.get():nullptr;const auto age=[&](uint64_t published){return now&&published&&now>=published?now-published:UINT64_MAX;};
        const bool sameReceiptScope=receipt&&SameEditorApplicationScope(receipt->scope,common),sameAppScope=app&&SameEditorApplicationScope(app->metadata,common);
        const bool identity=app&&receipt&&app->frame.nativeFrame==receipt->nativeFrame&&app->frame.sourceSequence==receipt->sourceSequence&&app->localPoseRevision&&app->localPoseRevision==receipt->cameraRevision;
        const bool projection=app&&receipt&&!std::memcmp(&app->projection.pose,&receipt->projection.pose,sizeof(XrPosef))&&!std::memcmp(&app->projection.fov,&receipt->projection.fov,sizeof(XrFovf));
        Log("[EditorNativeUiProof] count=%llu commonEpoch=%u commonSetup=%u commonController=%llx commonComponent=%llx commonKey=%llx rawControls=%llu rawOrigin=%llu rawBinding=%llu now=%llu receipt=%llx receiptEpoch=%u receiptSetup=%u receiptController=%llx receiptComponent=%llx receiptKey=%llx receiptEye=%u receiptFrame=%u receiptSource=%llu receiptRevision=%llu receiptPublished=%llu receiptAge=%llu submission=%llu queue=%llx backBuffer=%llx app=%llx appEpoch=%u appSetup=%u appEye=%u appFrame=%u appSource=%llu appRevision=%llu appKey=%llx appBinding=%llu appControls=%llu appOrigin=%llu appPublished=%llu appAge=%llu sameReceiptScope=%u sameAppScope=%u identityMatch=%u projectionMatch=%u; immutable receipt/raw fields only, registration not queried again, unknown age is UINT64_MAX\n",
            count,common.screenEpoch,common.setupID,common.controllerIdentity,common.componentIdentity,common.virtualCameraNameHash,controls,origin,binding,now,reinterpret_cast<uintptr_t>(receipt),receipt?receipt->scope.screenEpoch:0,receipt?receipt->scope.setupID:0,receipt?receipt->scope.controllerIdentity:0,receipt?receipt->scope.componentIdentity:0,receipt?receipt->scope.virtualCameraNameHash:0,receipt?receipt->eye:0,receipt?receipt->nativeFrame:0,receipt?receipt->sourceSequence:0,receipt?receipt->cameraRevision:0,receipt?receipt->scope.publishedAtMs:0,age(receipt?receipt->scope.publishedAtMs:0),receipt?receipt->uiSubmission:0,receipt?receipt->queue:0,receipt?receipt->backBuffer:0,reinterpret_cast<uintptr_t>(app),app?app->metadata.screenEpoch:0,app?app->metadata.setupID:0,app?app->frame.eye:0,app?app->frame.nativeFrame:0,app?app->frame.sourceSequence:0,app?app->localPoseRevision:0,app?app->source.cameraKey:0,app?app->source.bindingGeneration:0,app?app->editorControlRevision:0,app?app->projection.origin:0,app?app->metadata.publishedAtMs:0,age(app?app->metadata.publishedAtMs:0),unsigned(sameReceiptScope),unsigned(sameAppScope),unsigned(identity),unsigned(projection));
    }catch(...){}
}
}
struct OpenXRManager::EditorNativeUiSource {
    std::shared_ptr<const cvr::menu_preview::CameraApplication> application;
    std::optional<MenuUiSelection> packet;
    uint64_t presentSerial{};
};
bool OpenXRManager::PrepareMenuUiResize(){
    if(!cvr::menu_preview::UiStereoRequested())return true;
    ClearMenuUiPair();
    std::lock_guard captureLock(m_captureMutex);
    const auto required=m_captureContextFences.AllRequired();
    if(required==UINT64_MAX)return false;
    if(required){if(!m_captureFence||m_captureFence->GetCompletedValue()==UINT64_MAX)return false;
        if(m_captureFence->GetCompletedValue()<required){
            if(FAILED(m_captureFence->SetEventOnCompletion(required,m_captureFenceEvent))||WaitForSingleObject(m_captureFenceEvent,100)!=WAIT_OBJECT_0||m_captureFence->GetCompletedValue()==UINT64_MAX||m_captureFence->GetCompletedValue()<required)return false;
        }
    }
    for(unsigned slot=0;slot<3;++slot){auto* allocator=m_captureCmdAllocators[slot];auto* list=m_captureCmdLists[slot];
        if(!allocator&&!list)continue;if(!allocator||!list)return false;
        if(FAILED(allocator->Reset())||FAILED(list->Reset(allocator,nullptr)))return false;
        // Own lists are never replayed; actual last Execute fence is proven complete.
        cvr::gpu::ResetCommandResources(list);
        if(FAILED(list->Close()))return false;
    }
    cvr::gpu::CollectCommandResources();
    cvr::menu_preview::ClearUiBackBuffers();
    return true;
}
void OpenXRManager::ClearMenuUiPair(){
    std::array<uintptr_t,2> retired;
    std::shared_ptr<EditorNativeUiSource> retiredNative;
    std::shared_ptr<const cvr::menu_preview::UiFrameReceipt> retiredUi;
    {std::lock_guard lock(m_presentMutex);retired=m_menuUiPair.Clear();for(unsigned e=0;e<2;++e){m_menuUiCompletion[e]={};m_menuUiReceipt[e].reset();m_menuUiRawSceneSerial[e]=0;}
     retiredNative=std::move(m_editorNativeUiSource);retiredUi=std::move(m_editorNativeUiReceipt);}
    for(auto resource:retired)if(resource)ReleaseCapturedRead(reinterpret_cast<ID3D12Resource*>(resource),0);
}
std::optional<OpenXRManager::MenuUiSelection> OpenXRManager::AcquireEditorNativeMenuUiPair(const cvr::menu_preview::Lease& scope){
    try {
    using namespace cvr::menu_preview;
    const auto raw=AcquireEditorNativeScenePair(scope.metadata);
    const auto traceGate=[&](unsigned gate,const char* reason,bool source=false,bool receiptKnown=false,bool same=false,bool valid=false,bool proven=false){
        static std::array<std::atomic<uint32_t>,3> rows{};
        if(gate>=rows.size())return;
        const auto row=rows[gate].fetch_add(1,std::memory_order_relaxed);
        if(row<4||row==128||row==1024||row==8192)Log("[EditorNativeOutput] gate=%u reason=%s epoch=%u setup=%u frame=%u raw=%u source=%u receipt=%u sameApplication=%u uiValid=%u uiProven=%u count=%u; actual output admission observation only\n",
            gate,reason,scope.metadata.screenEpoch,scope.metadata.setupID,raw?raw->nativeFrame:0,unsigned(bool(raw)),unsigned(source),unsigned(receiptKnown),unsigned(same),unsigned(valid),unsigned(proven),row+1);
    };
    if(!raw||raw->origin!=GetTrackingOriginSerial()){traceGate(0,"raw-pair-or-origin");return {};}
    std::shared_ptr<EditorNativeUiSource> uiSource;std::shared_ptr<const UiFrameReceipt> receipt;uint64_t pool{};
    {std::lock_guard lock(m_presentMutex);uiSource=m_editorNativeUiSource;receipt=m_editorNativeUiReceipt;pool=m_captureResourceEpoch;}
    const bool sameApplication=uiSource&&receipt&&uiSource->application==receipt->application;
    const bool validUi=sameApplication&&MenuEditorUiValid(*receipt);
    uint64_t proofNow{};const bool provenUi=validUi&&cvr::xr::EditorNativeUiReceiptProven(*receipt,scope.metadata,raw->controlRevision,raw->origin,raw->bindingGeneration,(proofNow=PublicationNowMs()));
    if(!uiSource||!receipt||!sameApplication||!validUi||!provenUi){
        NoteEditorNativeUiProof(receipt.get(),scope.metadata,raw->controlRevision,raw->origin,raw->bindingGeneration,proofNow);
        traceGate(1,"primary-ui",bool(uiSource),bool(receipt),sameApplication,validUi,provenUi);return {};
    }
    traceGate(2,"raw-and-primary-ui-admitted",true,true,true,true,true);
    const auto pairScope=MenuUiScope(scope.metadata,pool);
    const auto cachePacket=[&](const MenuUiSelection& packet){
        std::shared_ptr<EditorNativeUiSource> latest;std::shared_ptr<const UiFrameReceipt> latestReceipt;
        {std::lock_guard lock(m_presentMutex);latest=m_editorNativeUiSource;latestReceipt=m_editorNativeUiReceipt;}
        if(!latest||!latestReceipt||latest->application!=latestReceipt->application||!MenuEditorUiValid(*latestReceipt)||
           !cvr::xr::EditorNativeUiReceiptProven(*latestReceipt,scope.metadata,raw->controlRevision,raw->origin,raw->bindingGeneration,PublicationNowMs()))return;
        auto replacement=std::make_shared<EditorNativeUiSource>(*latest);replacement->packet=packet;replacement->packet->editorUi=latestReceipt->editorUi;
        std::shared_ptr<EditorNativeUiSource> retired;
        {std::lock_guard lock(m_presentMutex);if(m_captureResourceEpoch==pool&&m_editorNativeUiSource==latest&&m_editorNativeUiReceipt==latestReceipt){retired=std::move(m_editorNativeUiSource);m_editorNativeUiSource=std::move(replacement);}}
        // The old packet's ReadLease may lock presentMutex during retirement.
    };
    if(uiSource->packet&&uiSource->packet->scope==pairScope&&
       uiSource->packet->rawSceneSerial[0]==raw->eyes[0]->serial&&uiSource->packet->rawSceneSerial[1]==raw->eyes[1]->serial){
        auto packet=*uiSource->packet;
        for(unsigned eye=0;eye<2;++eye)if(!packet.completion[eye].HasScheduledCopySource()||packet.completion[eye].fence->GetCompletedValue()==UINT64_MAX)return {};
        if(packet.editorUi!=receipt->editorUi){packet.editorUi=receipt->editorUi;packet.sourceSerial=uiSource->presentSerial;cachePacket(packet);}return packet;
    }
    // Accepted display A may pin two cells while an unaccepted cache B pins
    // the other two writable cells. Retire superseded B before reserving C;
    // clearing only the manager's pointer is insufficient while this function
    // still holds its old uiSource snapshot.
    std::shared_ptr<EditorNativeUiSource> retiredSuperseded,refreshedSource;
    std::shared_ptr<const UiFrameReceipt> refreshedReceipt;
    {std::lock_guard lock(m_presentMutex);
     auto replacement=cvr::xr::EditorNativeUiStateWithoutSupersededPacket(m_editorNativeUiSource,raw->eyes[0]->serial,raw->eyes[1]->serial);
     if(replacement!=m_editorNativeUiSource){retiredSuperseded=std::move(m_editorNativeUiSource);m_editorNativeUiSource=std::move(replacement);}
     refreshedSource=m_editorNativeUiSource;refreshedReceipt=m_editorNativeUiReceipt;}
    uiSource.reset();retiredSuperseded.reset(); // ReadLease retirement is outside presentMutex.
    uiSource=std::move(refreshedSource);receipt=std::move(refreshedReceipt);
    if(!uiSource||!receipt||uiSource->application!=receipt->application||!MenuEditorUiValid(*receipt)||
       !cvr::xr::EditorNativeUiReceiptProven(*receipt,scope.metadata,raw->controlRevision,raw->origin,raw->bindingGeneration,PublicationNowMs()))return {};
    // A concurrent UI/consumer publication may already have materialized C.
    if(uiSource->packet&&uiSource->packet->scope==pairScope&&
       uiSource->packet->rawSceneSerial[0]==raw->eyes[0]->serial&&uiSource->packet->rawSceneSerial[1]==raw->eyes[1]->serial){
        auto packet=*uiSource->packet;
        for(unsigned eye=0;eye<2;++eye)if(!packet.completion[eye].HasScheduledCopySource()||packet.completion[eye].fence->GetCompletedValue()==UINT64_MAX)return {};
        if(packet.editorUi!=receipt->editorUi){packet.editorUi=receipt->editorUi;packet.sourceSerial=uiSource->presentSerial;cachePacket(packet);}return packet;
    }
    std::unique_lock captureLock(m_captureMutex,std::try_to_lock);if(!captureLock.owns_lock())return {};
    if(!m_monoPool[0]||!m_captureFence||m_mainPoolProducer.unknown||!cvr::detail::g_orig_ExecuteCommandLists)return {};
    const auto destination=m_monoPool[0]->GetDesc();
    if(raw->eyes[0]->pixels.texture->GetDesc().Format!=raw->eyes[1]->pixels.texture->GetDesc().Format)return {};
    if(!m_editorSceneResample){m_editorSceneResample=std::make_unique<cvr::render::AerReconstructionPass>();
        if(!m_editorSceneResample->Initialize(m_d3dDevice)){m_editorSceneResample.reset();return {};}}
    if(!m_editorSceneResample->Prepare(raw->eyes[0]->pixels.texture->GetDesc().Format,destination.Format,cvr::xr::EditorSceneColorRoute(destination.Format)))return {};
    uintptr_t writeResources[2]{};cvr::capture::WriteRelease releaseWrites(m_captureLeases,m_presentMutex,writeResources);
    Microsoft::WRL::ComPtr<ID3D12Resource> snapshots[2];
    {std::lock_guard lock(m_presentMutex);if(m_captureResourceEpoch!=pool)return {};
     std::array<uintptr_t,cvr::capture::MainPoolMaximum> addresses{};for(unsigned n=0;n<m_monoPoolCount;++n)addresses[n]=uintptr_t(m_monoPool[n]);
     for(unsigned eye=0;eye<2;++eye){const auto slot=cvr::capture::ReserveMainWrite(std::span(addresses).first(m_monoPoolCount),uintptr_t(m_monoCapturedFrame.texture),m_monoPoolSlot,m_captureLeases);
        if(!slot)return {};m_monoPoolSlot=*slot;snapshots[eye]=m_monoPool[*slot];writeResources[eye]=uintptr_t(snapshots[eye].Get());}}
    m_captureAllocatorIndex=(m_captureAllocatorIndex+1)%3;
    const auto completed=m_captureFence->GetCompletedValue();
    if(completed==UINT64_MAX||!m_captureContextFences.CanReset(m_captureAllocatorIndex,completed)||m_captureFenceValue>=UINT64_MAX-1)return {};
    auto* allocator=m_captureCmdAllocators[m_captureAllocatorIndex];auto* list=m_captureCmdLists[m_captureAllocatorIndex];
    EditorUiPrivateCommandScope privateCommands;
    if(FAILED(allocator->Reset())||FAILED(list->Reset(allocator,nullptr)))return {};
    bool closed=false;struct CloseUnsubmitted {ID3D12GraphicsCommandList* list;bool& closed;~CloseUnsubmitted(){if(!closed)list->Close();}} closeUnsubmitted{list,closed};
    cvr::gpu::ResetCommandResources(list);
    std::array<cvr::render::AerRecordTicket,2> tickets{};bool executed=false;
    struct Cancel {cvr::render::AerReconstructionPass* pass;std::array<cvr::render::AerRecordTicket,2>& tickets;bool& executed;
        ~Cancel(){if(!executed)for(const auto& ticket:tickets)if(ticket)pass->CancelNeverExecuted(ticket);}} cancel{m_editorSceneResample.get(),tickets,executed};
    const auto value=m_captureFenceValue+1;
    for(unsigned eye=0;eye<2;++eye){cvr::xr::EditorSceneCaptureSource source{raw->eyes[eye]->pixels.texture.Get(),raw->eyes[eye],raw->eyes[eye]->application->projection};
        tickets[eye]=cvr::xr::RecordEditorSceneCapture(*m_editorSceneResample,list,source,snapshots[eye].Get(),m_captureFence,value);if(!tickets[eye])return {};}
    if(!(closed=SUCCEEDED(list->Close())))return {};
    cvr::framegen::CaptureCompletion completion;
    {std::lock_guard submission(cvr::camera::ImageSubmissionMutex());
     for(unsigned eye=0;eye<2;++eye){const auto& image=*raw->eyes[eye];const auto done=image.producerFence->GetCompletedValue();
        if(!EditorNativeSourceRegistered(scope.metadata,image.application->source)||!FreshPublication(image.publishedAtMs,PublicationNowMs())||
           !FreshPublication(image.application->metadata.publishedAtMs,PublicationNowMs())||!FreshPublication(scope.metadata.publishedAtMs,PublicationNowMs())||
           done==UINT64_MAX||done<image.producerValue||FAILED(m_d3dQueue->Wait(image.producerFence.Get(),image.producerValue)))return {};}
     if(!cvr::xr::EditorNativeUiReceiptProven(*receipt,scope.metadata,raw->controlRevision,raw->origin,raw->bindingGeneration,PublicationNowMs()))return {};
     ID3D12CommandList* lists[]{list};auto resources=cvr::gpu::PrepareCommandResources(1,lists);
     cvr::detail::g_orig_ExecuteCommandLists(m_d3dQueue,1,lists);executed=true;
     ++m_captureFenceValue;const auto signaled=m_d3dQueue->Signal(m_captureFence,value);
     for(const auto& ticket:tickets)m_editorSceneResample->SubmissionReported(ticket,true,SUCCEEDED(signaled));
     cvr::gpu::SubmitCommandResources(m_d3dQueue,std::move(resources));m_captureContextFences.Executed(m_captureAllocatorIndex,value,SUCCEEDED(signaled));
     m_mainPoolProducer.Executed(m_captureFence,value,SUCCEEDED(signaled));if(FAILED(signaled))return {};
     LARGE_INTEGER now{},frequency{};QueryPerformanceCounter(&now);QueryPerformanceFrequency(&frequency);
     completion=cvr::framegen::CaptureCompletion::AfterSignal(m_captureFence,value,signaled,now,frequency.QuadPart);}
    MenuUiSelection packet;packet.scope=pairScope;packet.sourceSerial=uiSource->presentSerial;packet.editorUiRequired=true;packet.editorUi=receipt->editorUi;
    packet.editorControlRevision=raw->controlRevision;packet.lease=std::make_shared<cvr::framegen::ReadLease>();packet.lease->owner=this;
    packet.lease->release=[](void* owner,ID3D12Resource* resource,uint64_t fence){static_cast<OpenXRManager*>(owner)->ReleaseCapturedRead(resource,fence);};
    {std::lock_guard lock(m_presentMutex);if(m_captureResourceEpoch!=pool)return {};
     for(unsigned eye=0;eye<2;++eye){m_captureLeases.EndWrite(uintptr_t(snapshots[eye].Get()));writeResources[eye]=0;
        if(!m_captureLeases.Pin(uintptr_t(snapshots[eye].Get())))return {};snapshots[eye]->AddRef();packet.lease->resources[eye]=snapshots[eye].Get();
        packet.color[eye]=snapshots[eye].Get();packet.completion[eye]=completion;packet.rawSceneSerial[eye]=raw->eyes[eye]->serial;
        packet.cameraRevision[eye]=raw->eyes[eye]->application->localPoseRevision;packet.projection[eye]=raw->eyes[eye]->application->projection;}}
    if(!cvr::xr::EditorOrbitViewsMatch(packet.projection[0].orbit,packet.projection[1].orbit,raw->origin,raw->controlRevision))return {};
    captureLock.unlock();cachePacket(packet);
    static unsigned rows{};if(rows++<8)Log("[EditorNativeUiPair] frame=%u raw=%llu/%llu uiSubmission=%llu control=%llu generation=%llu; real two-view raw, one actual UI receipt, managed capture lease\n",raw->nativeFrame,packet.rawSceneSerial[0],packet.rawSceneSerial[1],receipt->uiSubmission,raw->controlRevision,raw->bindingGeneration);
    return packet;
    }catch(...){return {};}
}
std::optional<OpenXRManager::MenuUiSelection> OpenXRManager::AcquireMenuUiPair(){
    if(!cvr::menu_preview::UiStereoRequested())return {};
    cvr::menu_preview::Lease scope;if(!cvr::menu_preview::TryAcquireCurrent(scope))return {};
    if(scope.metadata.screen==2&&cvr::menu_preview::EditorNativeApplicationsConfigured())return AcquireEditorNativeMenuUiPair(scope);
    // Returning to the original lane must release native cache publisher pins;
    // a last accepted display keeps its own shared ReadLease until replaced.
    std::shared_ptr<EditorNativeUiSource> retiredNative;std::shared_ptr<const cvr::menu_preview::UiFrameReceipt> retiredNativeUi;
    {std::lock_guard lock(m_presentMutex);retiredNative=std::move(m_editorNativeUiSource);retiredNativeUi=std::move(m_editorNativeUiReceipt);}
    MenuUiSelection selection;selection.lease=std::make_shared<cvr::framegen::ReadLease>();selection.lease->owner=this;
    selection.lease->release=[](void* owner,ID3D12Resource* resource,uint64_t fence){static_cast<OpenXRManager*>(owner)->ReleaseCapturedRead(resource,fence);};
    bool accepted=false,pairFound=false;std::optional<cvr::menu_preview::FreshnessSample> probePair;if(cvr::menu_preview::EditorFreshnessEnabled())probePair.emplace();
    {std::lock_guard lock(m_presentMutex);selection.scope=MenuUiScope(scope.metadata,m_captureResourceEpoch);
     if(const auto pair=m_menuUiPair.Read(selection.scope)){
        pairFound=true;if(cvr::menu_preview::EditorFreshnessEnabled()&&probePair){probePair->source=(*pair)[0].sourceSequence;probePair->otherSource=(*pair)[1].sourceSequence;probePair->revision=(*pair)[0].controlRevision;probePair->otherRevision=(*pair)[1].controlRevision;}
        selection.editorControlRevision=(*pair)[0].controlRevision;
        accepted=true;for(unsigned eye=0;eye<2;++eye){const auto& completion=m_menuUiCompletion[eye];
            if(!m_menuUiReceipt[eye]||!m_menuUiReceipt[eye]->cameraRevision||!MenuEditorUiValid(*m_menuUiReceipt[eye])||!completion.HasScheduledCopySource()||completion.fence->GetCompletedValue()==UINT64_MAX){accepted=false;break;}
            selection.projection[eye]=m_menuUiReceipt[eye]->projection;
            selection.rawSceneSerial[eye]=m_menuUiRawSceneSerial[eye];
            if(m_menuUiReceipt[eye]->editorUiRequired&&(!selection.rawSceneSerial[eye]||!selection.editorControlRevision)){accepted=false;break;}
            if(scope.metadata.screen==2&&(!selection.projection[eye].valid||selection.projection[eye].origin!=GetTrackingOriginSerial())){accepted=false;break;}
            auto* resource=reinterpret_cast<ID3D12Resource*>((*pair)[eye].resource);
            selection.sourceSerial=std::max(selection.sourceSerial,(*pair)[eye].serial);
            if(!m_captureLeases.Pin(uintptr_t(resource))){accepted=false;break;}
            resource->AddRef();selection.color[eye]=resource;selection.completion[eye]=completion;selection.lease->resources[eye]=resource;selection.cameraRevision[eye]=m_menuUiReceipt[eye]?m_menuUiReceipt[eye]->cameraRevision:0;
        }
        if(accepted){
            const auto& left=*m_menuUiReceipt[0];const auto& right=*m_menuUiReceipt[1];
            const auto required=cvr::xr::EditorUiPairRequirement(left.editorUiRequired,right.editorUiRequired);
            selection.editorUiRequired=required.value_or(true);
            if(!required)accepted=false;
            else if(!cvr::xr::EditorOrbitViewsMatch(selection.projection[0].orbit,selection.projection[1].orbit,GetTrackingOriginSerial(),selection.editorControlRevision))accepted=false;
            else if(selection.editorUiRequired)selection.editorUi=
                left.editorUi->sourceSequence>right.editorUi->sourceSequence?left.editorUi:right.editorUi;
        }
     }else if(scope.metadata.screen==2){static unsigned rows{},lastMask=~0u;const auto& cells=m_menuUiPair.Cells();const unsigned mask=(cells[0]?1u:0u)|(cells[1]?2u:0u);if(mask!=lastMask&&rows++<8){lastMask=mask;Log("[MenuPreviewUiPair] read-miss current=%u/%u/%llx/%llx/%llx/%llu left=%u/%u/%llx/%llx/%llx/%llu:%llx right=%u/%u/%llx/%llx/%llx/%llu:%llx\n",selection.scope.epoch,selection.scope.setup,selection.scope.owner,selection.scope.component,selection.scope.camera,selection.scope.pool,cells[0].scope.epoch,cells[0].scope.setup,cells[0].scope.owner,cells[0].scope.component,cells[0].scope.camera,cells[0].scope.pool,cells[0].resource,cells[1].scope.epoch,cells[1].scope.setup,cells[1].scope.owner,cells[1].scope.component,cells[1].scope.camera,cells[1].scope.pool,cells[1].resource);}}
     }
    if(scope.metadata.screen==2){static unsigned rows{};static int previous=-1;if(rows<8&&previous!=int(accepted)){++rows;previous=int(accepted);Log("[MenuPreviewUiPair] acquire accepted=%u left=%p right=%p revision=%llu/%llu\n",unsigned(accepted),selection.color[0],selection.color[1],selection.cameraRevision[0],selection.cameraRevision[1]);}}
    if(cvr::menu_preview::EditorFreshnessEnabled()&&scope.metadata.screen==2&&probePair){
        probePair->epoch=scope.metadata.screenEpoch;probePair->setup=scope.metadata.setupID;probePair->control=selection.editorControlRevision;probePair->origin=selection.projection[0].origin;probePair->rawLeft=selection.rawSceneSerial[0];probePair->rawRight=selection.rawSceneSerial[1];probePair->flags=unsigned(pairFound)|(unsigned(selection.editorUiRequired)<<1);
        cvr::menu_preview::NoteEditorFreshness(accepted?cvr::menu_preview::FreshnessEvent::PairAccept:pairFound?cvr::menu_preview::FreshnessEvent::PairAdmissionReject:cvr::menu_preview::FreshnessEvent::PairReadMiss,cvr::menu_preview::FreshnessStage::Pair,*probePair);
    }
    if(!accepted)return {};return selection; // rejected lease destructs outside presentMutex
}
bool OpenXRManager::CaptureMenuUiPresentedFrame(ID3D12Resource* backBuffer,const D3D12_RESOURCE_DESC& description,uint64_t serial){
    if(!cvr::menu_preview::UiStereoRequested()||!backBuffer||!m_d3dQueue)return false;
    const auto receipt=cvr::menu_preview::AcquireUiFrameForCapture(backBuffer,m_d3dQueue);
    if(!receipt||receipt->eye>1||!receipt->cameraRevision||!MenuEditorUiValid(*receipt))return false;
    if(receipt->scope.screen==2&&cvr::menu_preview::EditorNativeApplicationsConfigured()){
        try {
        const auto& app=receipt->application;
        if(!app||!receipt->editorUiRequired||!cvr::xr::EditorNativeUiReceiptProven(*receipt,receipt->scope,app->editorControlRevision,
            app->projection.origin,app->source.bindingGeneration,cvr::menu_preview::PublicationNowMs()))return false;
        auto next=std::make_shared<EditorNativeUiSource>();next->application=app;next->presentSerial=serial;
        std::shared_ptr<EditorNativeUiSource> retiredNative;std::shared_ptr<const cvr::menu_preview::UiFrameReceipt> retiredUi;
        std::array<uintptr_t,2> retired{};
        std::lock_guard captureLock(m_captureMutex);if(!EnsureMonoCaptureResource(description)||m_mainPoolProducer.unknown)return false;
        {std::lock_guard submission(cvr::camera::ImageSubmissionMutex());
         if(!cvr::menu_preview::ValidateUiFrameForCapture(*receipt,backBuffer,m_d3dQueue))return false;
         std::lock_guard lock(m_presentMutex);cvr::menu_preview::Lease current;
         if(!cvr::menu_preview::TryAcquireCurrent(current)||!cvr::menu_preview::SameEditorApplicationScope(current.metadata,receipt->scope)||
            !cvr::menu_preview::EditorNativeSourceRegistered(current.metadata,app->source))return false;
         if(m_editorNativeUiSource&&m_editorNativeUiSource->application&&
            m_editorNativeUiSource->application->source==app->source&&
            cvr::menu_preview::SameEditorApplicationScope(m_editorNativeUiSource->application->metadata,app->metadata)&&
            m_editorNativeUiSource->application->projection.origin==app->projection.origin)next->packet=m_editorNativeUiSource->packet;
         retiredNative=std::move(m_editorNativeUiSource);retiredUi=std::move(m_editorNativeUiReceipt);
         m_editorNativeUiSource=std::move(next);m_editorNativeUiReceipt=receipt;
         retired=m_menuUiPair.Clear();for(unsigned eye=0;eye<2;++eye){m_menuUiReceipt[eye].reset();m_menuUiCompletion[eye]={};m_menuUiRawSceneSerial[eye]=0;}}
        for(auto resource:retired)if(resource)ReleaseCapturedRead(reinterpret_cast<ID3D12Resource*>(resource),0);
        return true; // Only the real primary UI publication; no right BB/UI receipt.
        }catch(...){return false;}
    }
    const bool rawRequired=cvr::menu_preview::EditorUiSeparationEnabled()&&receipt->scope.screen==2&&receipt->editorUiRequired;
    const auto source=cvr::xr::SelectEditorSceneCapture(rawRequired,backBuffer,*receipt,
        rawRequired?cvr::menu_preview::AcquireEditorSceneImage(*receipt):nullptr,cvr::menu_preview::PublicationNowMs());
    if(!source.texture){if(cvr::menu_preview::EditorFreshnessEnabled()&&receipt->scope.screen==2){cvr::menu_preview::FreshnessSample p;p.epoch=receipt->scope.screenEpoch;p.setup=receipt->scope.setupID;p.source=receipt->sourceSequence;p.revision=receipt->cameraRevision;p.nativeFrame=receipt->nativeFrame;p.eye=receipt->eye;cvr::menu_preview::NoteEditorFreshness(cvr::menu_preview::FreshnessEvent::RawUnavailable,cvr::menu_preview::FreshnessStage::Pair,p);}return false;} // Required raw unavailable: preserve the accepted pair and MAIN pool.
    const auto privateExecute=source.scene?cvr::detail::g_orig_ExecuteCommandLists:nullptr;
    if(source.scene&&!privateExecute)return false;
    std::shared_ptr<const cvr::menu_preview::UiFrameReceipt> pairReceipt=receipt;
    if(source.scene){auto rawReceipt=std::make_shared<cvr::menu_preview::UiFrameReceipt>(*receipt);
        rawReceipt->projection=source.projection;pairReceipt=std::move(rawReceipt);}
    std::unique_lock captureLock(m_captureMutex);
    if(source.scene){
        if(!m_editorSceneResample){m_editorSceneResample=std::make_unique<cvr::render::AerReconstructionPass>();
            if(!m_editorSceneResample->Initialize(m_d3dDevice)){m_editorSceneResample.reset();return false;}}
        if(!m_editorSceneResample->Prepare(source.texture->GetDesc().Format,description.Format,
            cvr::xr::EditorSceneColorRoute(description.Format)))return false;
    }
    if(!EnsureMonoCaptureResource(description)||m_mainPoolProducer.unknown)return false;
    cvr::menu_preview::UiPairScope scope;
    {std::lock_guard lock(m_presentMutex);scope=MenuUiScope(receipt->scope,m_captureResourceEpoch);if(m_menuUiPair.HasSource(scope,receipt->eye,receipt->sourceSequence))return false;}
    uintptr_t writeResources[2]{};cvr::capture::WriteRelease releaseWrites(m_captureLeases,m_presentMutex,writeResources);
    Microsoft::WRL::ComPtr<ID3D12Resource> snapshot;
    {std::lock_guard lock(m_presentMutex);std::array<uintptr_t,cvr::capture::MainPoolMaximum> addresses{};
     for(unsigned i=0;i<m_monoPoolCount;++i)addresses[i]=uintptr_t(m_monoPool[i]);
     const auto slot=cvr::capture::ReserveMainWrite(std::span(addresses).first(m_monoPoolCount),uintptr_t(m_monoCapturedFrame.texture),m_monoPoolSlot,m_captureLeases);
     if(!slot)return false;m_monoPoolSlot=*slot;snapshot=m_monoPool[*slot];writeResources[0]=uintptr_t(snapshot.Get());}
    m_captureAllocatorIndex=(m_captureAllocatorIndex+1)%3;
    const auto requiredFence=m_captureContextFences.Required(m_captureAllocatorIndex);
    if(requiredFence==UINT64_MAX||m_captureFence->GetCompletedValue()==UINT64_MAX)return false;
    if(!m_captureContextFences.CanReset(m_captureAllocatorIndex,m_captureFence->GetCompletedValue())){
        if(FAILED(m_captureFence->SetEventOnCompletion(requiredFence,m_captureFenceEvent))||WaitForSingleObject(m_captureFenceEvent,100)!=WAIT_OBJECT_0||!m_captureContextFences.CanReset(m_captureAllocatorIndex,m_captureFence->GetCompletedValue()))return false;
    }
    auto* allocator=m_captureCmdAllocators[m_captureAllocatorIndex];auto* list=m_captureCmdLists[m_captureAllocatorIndex];
    std::optional<cvr::menu_preview::EditorUiPrivateCommandScope> privateScope;
    if(source.scene)privateScope.emplace();
    if(FAILED(allocator->Reset())||FAILED(list->Reset(allocator,nullptr)))return false;
    if(source.scene)cvr::gpu::ResetCommandResources(list); // private Reset bypasses the generic hook
    if(m_captureFenceValue>=UINT64_MAX-1)return false;
    const auto copyFenceValue=m_captureFenceValue+1;
    cvr::render::AerRecordTicket rawTicket;bool rawExecuted=false;
    struct CancelRaw {cvr::render::AerReconstructionPass* pass; cvr::render::AerRecordTicket& ticket;bool& executed;
        ~CancelRaw(){if(pass&&ticket&&!executed)pass->CancelNeverExecuted(ticket);}} cancelRaw{source.scene?m_editorSceneResample.get():nullptr,rawTicket,rawExecuted};
    if(source.scene){rawTicket=cvr::xr::RecordEditorSceneCapture(*m_editorSceneResample,list,source,snapshot.Get(),m_captureFence,copyFenceValue);if(!rawTicket)return false;}
    else {if(!cvr::gpu::KeepCommandResources(list,{backBuffer,snapshot.Get()}))return false;
     cvr::stereo::probe::Scope internal;
     D3D12_RESOURCE_BARRIER pre[2]{};pre[0].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
     pre[0].Transition={backBuffer,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_SOURCE};pre[1]=fgtrace::MainCaptureWriteBarrier(snapshot.Get());
     list->ResourceBarrier(2,pre);list->CopyResource(snapshot.Get(),backBuffer);
     D3D12_RESOURCE_BARRIER post[2]{};post[0]=fgtrace::MainCaptureReadyBarrier(snapshot.Get());post[1]=pre[0];post[1].Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;post[1].Transition.StateAfter=D3D12_RESOURCE_STATE_PRESENT;list->ResourceBarrier(2,post);}
    if(FAILED(list->Close()))return false;
    cvr::framegen::CaptureCompletion completion;
    {std::lock_guard submission(cvr::camera::ImageSubmissionMutex());
     if(!cvr::menu_preview::ValidateUiFrameForCapture(*receipt,backBuffer,m_d3dQueue))return false;
     if(source.scene){const auto current=cvr::menu_preview::AcquireEditorSceneImage(*receipt);
        if(current!=source.scene||!cvr::xr::EditorSceneMatchesReceipt(*source.scene,*receipt,cvr::menu_preview::PublicationNowMs())||
           source.scene->producerFence->GetCompletedValue()==UINT64_MAX||
           FAILED(m_d3dQueue->Wait(source.scene->producerFence.Get(),source.scene->producerValue)))return false;}
     ID3D12CommandList* lists[]{list};
     auto resources=source.scene?cvr::gpu::PrepareCommandResources(1,lists):cvr::gpu::ResourceSubmission{};
     if(source.scene)privateExecute(m_d3dQueue,1,lists);else m_d3dQueue->ExecuteCommandLists(1,lists);
     rawExecuted=bool(source.scene);
     const auto value=++m_captureFenceValue;const auto signaled=m_d3dQueue->Signal(m_captureFence,value);
     if(source.scene){m_editorSceneResample->SubmissionReported(rawTicket,true,SUCCEEDED(signaled));cvr::gpu::SubmitCommandResources(m_d3dQueue,std::move(resources));}
     m_captureContextFences.Executed(m_captureAllocatorIndex,value,SUCCEEDED(signaled));
     m_mainPoolProducer.Executed(m_captureFence,value,SUCCEEDED(signaled));if(FAILED(signaled))return false;
     LARGE_INTEGER now{},frequency{};QueryPerformanceCounter(&now);QueryPerformanceFrequency(&frequency);completion=cvr::framegen::CaptureCompletion::AfterSignal(m_captureFence,value,signaled,now,frequency.QuadPart);}
    std::array<uintptr_t,2> retired{};bool published=false;
    {std::lock_guard lock(m_presentMutex);cvr::menu_preview::Lease current;
     if(cvr::menu_preview::TryAcquireCurrent(current)&&MenuUiScope(current.metadata,m_captureResourceEpoch)==scope){
        m_captureLeases.EndWrite(uintptr_t(snapshot.Get()));writeResources[0]=0;
        if(m_captureLeases.Pin(uintptr_t(snapshot.Get()))){snapshot->AddRef();
         retired=m_menuUiPair.Publish({scope,uintptr_t(snapshot.Get()),0,receipt->sourceSequence,receipt->uiSubmission,serial,receipt->eye,receipt->nativeFrame,source.scene?source.scene->application->editorControlRevision:0});
         published=m_menuUiPair.HasSource(scope,receipt->eye,receipt->sourceSequence);
         if(published){for(unsigned e=0;e<2;++e)if(retired[e]){m_menuUiReceipt[e].reset();m_menuUiCompletion[e]={};m_menuUiRawSceneSerial[e]=0;}m_menuUiReceipt[receipt->eye]=pairReceipt;m_menuUiCompletion[receipt->eye]=completion;m_menuUiRawSceneSerial[receipt->eye]=source.scene?source.scene->serial:0;}
         else{m_captureLeases.Unpin(uintptr_t(snapshot.Get()));snapshot->Release();}
        }
     }}
    for(auto resource:retired)if(resource)ReleaseCapturedRead(reinterpret_cast<ID3D12Resource*>(resource),0);
    captureLock.unlock();
    if(published){cvr::menu_preview::AcknowledgeUiCapture(*receipt);
     if(source.scene){static std::atomic<uint64_t> rows{};const auto row=rows.fetch_add(1)+1;if(row<=8||row%180==0){const auto sd=source.texture->GetDesc(),td=snapshot->GetDesc();Log("[MenuPreviewRawScene] published serial=%llu eye=%u epoch=%u setup=%u nativeFrame=%u source=%llu revision=%llu controlRevision=%llu raw=%p size=%llux%u format=%u destination=%p size=%llux%u format=%u producerUI=%llu receiptUI=%llu producerFence=%p/%llu captureFence=%llu; full raw image, independent UI retained\n",source.scene->serial,receipt->eye,receipt->scope.screenEpoch,receipt->scope.setupID,receipt->nativeFrame,receipt->sourceSequence,receipt->cameraRevision,source.scene->application->editorControlRevision,source.texture,sd.Width,sd.Height,unsigned(sd.Format),snapshot.Get(),td.Width,td.Height,unsigned(td.Format),source.scene->producerUiSubmission,receipt->uiSubmission,source.scene->producerFence.Get(),source.scene->producerValue,completion.fenceValue);}}
     else Log("[MenuPreviewUiPair] published eye=%u epoch=%u setup=%u frame=%u source=%llu uiSubmission=%llu cameraRevision=%llu snapshot=%p fence=%llu; full UI copied on actual game queue\n",receipt->eye,receipt->scope.screenEpoch,receipt->scope.setupID,receipt->nativeFrame,receipt->sourceSequence,receipt->uiSubmission,receipt->cameraRevision,snapshot.Get(),completion.fenceValue);}
    return published;
}
bool OpenXRManager::CaptureMonoPresentedFrame(ID3D12Resource* backBuffer, const D3D12_RESOURCE_DESC& sourceDesc, uint64_t serial,
    const XrPosef poses[2], const XrFovf fovs[2], const bool hasView[2],const XrPosef headCenters[2],
    const cvr::render::NativeColorCapture* nativeColor) {
    using CaptureReason=cvr::framegen::AerCaptureReason;
    cvr::framegen::AerCaptureRecord captureDiagnostic;captureDiagnostic.captureSerial=serial;
    CyberpunkVR_AerCaptureDebug.Record(CaptureReason::Attempt,captureDiagnostic);
    const auto captureFailed=[&](CaptureReason reason) {
        CyberpunkVR_AerCaptureDebug.Record(reason,captureDiagnostic);return false;
    };
    uint64_t catalogueAttempt{};
    {std::lock_guard lock(m_presentMutex);catalogueAttempt=m_mainCaptureCatalogue.Generation();}
    if (!backBuffer || !hasView[0] || !hasView[1]) {
        CVR_DIAGNOSTIC(CyberpunkVR_DebugCapSkipNoView.fetch_add(1, std::memory_order_relaxed));
        return captureFailed(CaptureReason::NoView);
    }
    const auto orderedSource=nativeColor?nativeColor->pose.mainEyeSource:cvr::framegen::MainEyeSourceReceipt{};
    const bool ordered=bool(orderedSource)&&cvr::framegen::GetSettings().aerV2;
    static_assert(cvr::capture::MainEyeQueueSlots==cvr::framegen::AerRealPresenter::SourceSlotsPerEye);

    std::lock_guard<std::mutex> captureLock(m_captureMutex);
    uintptr_t writeResources[2]{};
    cvr::capture::WriteRelease releaseWrites(m_captureLeases,m_presentMutex,writeResources);
    if (!EnsureMonoCaptureResource(sourceDesc)) {
        CVR_DIAGNOSTIC(CyberpunkVR_DebugCapSkipNoRes.fetch_add(1, std::memory_order_relaxed));
        return captureFailed(CaptureReason::NoResource);
    }

    // Original26.03 has three physical source slots per eye and, by default,
    // waits for consumer space (App freewheeling is off). Unclaimed publications
    // and claimed read leases both protect their cells. AER already owns a
    // separate display-rate submit thread; never wait on it under presentMutex.
    ID3D12Resource* snapshot = nullptr;
    decltype(m_mainCaptureCatalogue)::Key catalogueWrite;
    {
        std::lock_guard lock(m_presentMutex);
        m_mainCaptureCatalogue.SetOrderedMode(ordered);
        if(ordered){m_mainCaptureCatalogue.DiscardOutsideScope(orderedSource);catalogueAttempt=m_mainCaptureCatalogue.Generation();}
    }
    for(;;){
        if(ordered&&(uint32_t(orderedSource.physicalEye)>1||!UseThreadedSubmit()||m_stopFrameThread.load(std::memory_order_acquire)||
           !m_sessionRunning.load(std::memory_order_acquire)||!m_monoSubmitEnabled.load(std::memory_order_acquire)||
           !cvr::framegen::GetSettings().aerV2||!cvr::camera::ValidateMainEyeSourceReceipt(orderedSource)||
           !m_d3dDevice||FAILED(m_d3dDevice->GetDeviceRemovedReason())))break;
        bool duplicate=false;std::vector<ID3D12Resource*> retired;
        {
        std::unique_lock<std::mutex> lock(m_presentMutex);
        CollectCapturedReadsLocked(retired);
        captureDiagnostic.rendererEpoch=m_captureResourceEpoch;
        captureDiagnostic.poolCount=m_monoPoolCount;
        captureDiagnostic.captureFenceValue=m_captureFenceValue;
        captureDiagnostic.captureFenceCompleted=m_captureFence?m_captureFence->GetCompletedValue():0;
        captureDiagnostic.producerUnknown=m_mainPoolProducer.unknown?1:0;
        std::array<uintptr_t,cvr::capture::MainPoolMaximum> addresses{};
        for(uint32_t i=0;i<m_monoPoolCount;++i) {
            addresses[i]=reinterpret_cast<uintptr_t>(m_monoPool[i]);
            if(m_monoPool[i]==m_monoCapturedFrame.texture)captureDiagnostic.publishedSlot=i;
            if(m_captureLeases.Readers(addresses[i]))captureDiagnostic.pinnedMask|=uint64_t{1}<<i;
            if(m_captureLeases.Writing(addresses[i]))captureDiagnostic.writingMask|=uint64_t{1}<<i;
            if(ordered&&(!cvr::capture::MainEyeQueueSlot(i,uint32_t(orderedSource.physicalEye))||
               !m_mainCaptureCatalogue.CanOverwrite(i)))addresses[i]=0;
        }
        duplicate=ordered&&(m_monoPoolCount<2*cvr::capture::MainEyeQueueSlots||m_mainCaptureCatalogue.HasSource(orderedSource));
        if(!duplicate)if(const auto slot=cvr::capture::ReserveMainWrite(std::span(addresses).first(m_monoPoolCount),
                reinterpret_cast<uintptr_t>(m_monoCapturedFrame.texture),m_monoPoolSlot,m_captureLeases)) {
            m_monoPoolSlot=*slot;snapshot=m_monoPool[*slot];
            catalogueWrite=m_mainCaptureCatalogue.BeginWrite(*slot,catalogueAttempt);
            captureDiagnostic.writerSlot=*slot;
            writeResources[0]=reinterpret_cast<uintptr_t>(snapshot);
        }
        if (snapshot) {
            snapshot->AddRef();
        }
        if(ordered&&!snapshot&&!duplicate)m_mainCaptureAvailable.wait_for(lock,std::chrono::milliseconds(10));
        }
        for(auto* item:retired)item->Release();
        if(snapshot||!ordered||duplicate)break;
    }
    if (!snapshot) {
        static const bool historyDiagnostic=[] {char flag[8]{};return GetEnvironmentVariableA("CPVR_FG_HISTORY_DIAG",flag,sizeof(flag))==1&&flag[0]=='1';}();
        static uint64_t busySamples{};
        if(historyDiagnostic && (++busySamples<=6||busySamples%180==0))
            Log("[MAIN-POOL-BUSY] n=%llu capture=%llu slots=%u publishedSlot=%llu pinnedMask=0x%llx writingMask=0x%llx captureFence=%llu completed=%llu producerUnknown=%llu ordered=%u\n",
                (unsigned long long)busySamples,(unsigned long long)serial,unsigned(captureDiagnostic.poolCount),
                (unsigned long long)captureDiagnostic.publishedSlot,(unsigned long long)captureDiagnostic.pinnedMask,
                (unsigned long long)captureDiagnostic.writingMask,(unsigned long long)captureDiagnostic.captureFenceValue,
                (unsigned long long)captureDiagnostic.captureFenceCompleted,(unsigned long long)captureDiagnostic.producerUnknown,unsigned(ordered));
        CVR_DIAGNOSTIC(CyberpunkVR_DebugCapSkipNoSlot.fetch_add(1, std::memory_order_relaxed));
        return captureFailed(CaptureReason::NoSlot);
    }

    m_captureAllocatorIndex = (m_captureAllocatorIndex + 1) % 3;
    ID3D12CommandAllocator* currentAllocator = m_captureCmdAllocators[m_captureAllocatorIndex];
    
    const auto requiredFence=m_captureContextFences.Required(m_captureAllocatorIndex);
    if(requiredFence==UINT64_MAX||m_captureFence->GetCompletedValue()==UINT64_MAX){snapshot->Release();return captureFailed(CaptureReason::FenceTimeout);}
    if (!m_captureContextFences.CanReset(m_captureAllocatorIndex,m_captureFence->GetCompletedValue())) {
        if(FAILED(m_captureFence->SetEventOnCompletion(requiredFence,m_captureFenceEvent))){snapshot->Release();return captureFailed(CaptureReason::FenceTimeout);}
        // BOUNDED. This runs on the game's Present thread, so an INFINITE wait turns any
        // GPU hiccup into a hung game. Skipping one capture costs nothing now: the submit
        // side re-sends the frame it already holds, with the pose that belongs to it.
        // TIMED, not just counted. This blocks the game's Present thread, so the duration IS the
        // cost -- and a wait that succeeds after 30 ms does more damage than one that times out,
        // because it delays the publish without being recorded as a skip.
        const bool diagnostics=cvr::RuntimeDiagnosticsEnabled();
        const double fenceEnterMs = diagnostics ? XrDiagNowMs() : 0;
        const DWORD fenceRes = WaitForSingleObject(m_captureFenceEvent, 100);
        if(diagnostics) {
        const double fenceMs = XrDiagNowMs() - fenceEnterMs;
        CVR_DIAGNOSTIC(CyberpunkVR_DebugCapFenceWaits.fetch_add(1, std::memory_order_relaxed));
        const unsigned long long fenceUs = (unsigned long long)(fenceMs * 1000.0);
        CVR_DIAGNOSTIC(CyberpunkVR_DebugCapFenceUsSum.fetch_add(fenceUs, std::memory_order_relaxed));
        unsigned long long prevMax =
            CyberpunkVR_DebugCapFenceUsMax.load(std::memory_order_relaxed);
        while (fenceUs > prevMax &&
               !CyberpunkVR_DebugCapFenceUsMax.compare_exchange_weak(
                   prevMax, fenceUs, std::memory_order_relaxed)) {
        }
        }
        if (fenceRes != WAIT_OBJECT_0 || !m_captureContextFences.CanReset(m_captureAllocatorIndex,m_captureFence->GetCompletedValue())) {
            CVR_DIAGNOSTIC(CyberpunkVR_DebugCapSkipFence.fetch_add(1, std::memory_order_relaxed));
            snapshot->Release();
            return captureFailed(CaptureReason::FenceTimeout);
        }
    }

    ID3D12GraphicsCommandList* m_captureCmdList = m_captureCmdLists[m_captureAllocatorIndex];

    if (FAILED(currentAllocator->Reset()) || FAILED(m_captureCmdList->Reset(currentAllocator, nullptr))) {
        Log("OpenXRManager: Failed to reset mono capture command list\n");
        snapshot->Release();
        return captureFailed(CaptureReason::ResetFailed);
    }

    // The selected engine source can be retired by a graph/PPD rebuild before
    // the capture finishes on GPU. Its local selection reference is CPU-only;
    // use the existing immutable submission owner for this actual copy too.
    if(nativeColor && !cvr::gpu::KeepCommandResources(m_captureCmdList,{backBuffer,snapshot,nativeColor->lifetime.Get()})) {
        snapshot->Release();return captureFailed(CaptureReason::NoResource);
    }
    const auto sourceState=nativeColor?nativeColor->state:D3D12_RESOURCE_STATE_PRESENT;
    auto gpuProfile = cvr::gpu::profile::Begin(m_captureCmdList);
    {
    cvr::gpu::profile::Scope gpuScope(gpuProfile, cvr::gpu::profile::Stage::CaptureMain);
    D3D12_RESOURCE_BARRIER barriers[2] = {};
    UINT barrierCount = 0;

    barriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[barrierCount].Transition.pResource = backBuffer;
    barriers[barrierCount].Transition.StateBefore = sourceState;
    barriers[barrierCount].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[barrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ++barrierCount;

    barriers[barrierCount++]=fgtrace::MainCaptureWriteBarrier(snapshot);

    m_captureCmdList->ResourceBarrier(barrierCount, barriers);
    m_captureCmdList->CopyResource(snapshot, backBuffer);

    D3D12_RESOURCE_BARRIER afterCopy[2] = {};
    afterCopy[0]=fgtrace::MainCaptureReadyBarrier(snapshot);
    afterCopy[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    afterCopy[1].Transition.pResource = backBuffer;
    afterCopy[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    afterCopy[1].Transition.StateAfter = sourceState;
    afterCopy[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_captureCmdList->ResourceBarrier(2, afterCopy);
    }

    // ---- right eye: the VRCAM view, converted here and now ---------------------------------
    //
    // Same list, same fence, same moment as MAIN's copy above -- which is the whole point.
    // sync_stereo's snapshot belongs to the engine and the desktop mirror also reads it from
    // another queue; the earlier attempt to sample it from the submit thread at display rate
    // put two queues on one resource and hung the GPU on the first VRCAM frame. Here the
    // engine's frame is already submitted and we are on the Present thread, so this is the
    // one point where reading it is ours to do.
    //
    // It cannot be copied the way MAIN is: MAIN's backbuffer is sRGB-encoded UNORM bytes that
    // match the eye swapchain bit for bit, while this is RenderFinal2D's output -- 2444x2444
    // R11G11B10_FLOAT holding LINEAR values (confirmed live: "rtvpick ... fmt=26"). So it goes
    // through ColorBlit into a target typed as the eye swapchain's own format: the sampler
    // resizes, and writing through an _UNORM_SRGB RTV makes the hardware do the linear->sRGB
    // encode. What lands in the eye slot is then byte-identical in kind to MAIN's snapshot,
    // and the submit copies it with the same plain CopyResource.
    // Sizes and formats exactly as the version that worked (recovered from this project's own
    // EnsureStereoCaptureResources / CaptureStereoPresentedFrame):
    //
    //   size    = MAIN's, not VRCAM's and not the XR image's. Eye 1 is BLITTED, not copied,
    //             so it is sized to MAIN and the blit rescales VRCAM if the two disagree.
    //             They normally match -- the launcher pick drives both the MAIN resolution
    //             override and the VRCAM component -- and a mismatch (MAIN 2560 against a
    //             vrcam_2444x2444 pick) just costs a rescale.
    //   resource= R8G8B8A8_TYPELESS. D3D12 only allows the UNORM -> UNORM_SRGB view cast
    //             through a typeless resource, so this is what makes the encode legal.
    //   view    = R8G8B8A8_UNORM_SRGB. The sRGB RTV makes the hardware encode the linear
    //             VRCAM colour on write -- exact and free, unlike a pow() in the shader.
    //
    // Deriving these from the XR image instead is what killed the GPU twice: that resource is
    // typeless, and a typeless RTV is invalid.
    bool vrcamEyeCaptured = false;
    ID3D12Resource* vrcamCaptureSource=nullptr;
    D3D12_RESOURCE_DESC vrcamRenderedDesc{};
    const uint32_t eyeW = static_cast<uint32_t>(sourceDesc.Width);
    const uint32_t eyeH = sourceDesc.Height;
    // NOT IN A MENU. The right eye below is VRCAM's view of the WORLD; the menu is not in it,
    // because a menu is drawn as one mono surface into the backbuffer. Taking VRCAM here
    // anyway gives the left eye the menu and the right eye a frozen street, which is the
    // reported "the headset shows VRCAM and not the menu": with the two eyes disagreeing
    // that completely, the scene is what reads.
    // Skipping it leaves both eyes on the backbuffer snapshot, i.e. the menu, mono -- which is
    // what a mono surface should look like.
    const bool menuOpen = (GetMenuRectMode() != 0) || (GetMenuMode() != 0);
    if (CyberpunkVR_StereoSubmit && eyeW && eyeH && !(menuOpen && CyberpunkVR_MonoMenu)) {
        ID3D12Resource* vrcamSrc = CyberpunkVR_GetVrcamEyeTextureFresh();
        vrcamCaptureSource=vrcamSrc;
        if(vrcamSrc)vrcamRenderedDesc=vrcamSrc->GetDesc();
        // THE AGE OF THE SECOND EYE'S CONTENT, which is a different quantity from everything else
        // measured so far and the only one that can be asymmetric between the eyes.
        //
        // [xreye] counts whether the eye got its own IMAGE, by serial. But the serial is stamped when
        // the BLIT runs, and the blit copies whatever the second view last produced -- so a frame the
        // engine did not re-render for that view is copied again with a FRESH serial. Paired by stamp,
        // stale by content. That is the one shape which shows in one eye only, and it is what
        // GetVrcamEyeTextureFresh already computes for its own staleness gate.
        //
        // Behind CyberpunkVR_XrDeepDiag, like the rest of that instrument. The staleness GATE itself is
        // not gated -- it decides whether this eye is submitted at all -- only the accumulation for
        // [xrage] is.
        if (CyberpunkVR_XrDeepDiag) {
            // MICROSECONDS. The millisecond value comes off a 15.6 ms clock and reported max 16.0 ms
            // in every window -- one tick, not a measurement, and useless at the 14 ms scale that
            // matters. See StableNowMs in src/Stereo/Capture.cpp.
            const unsigned long long ageUs = CyberpunkVR_DebugVrcamEyeAgeUs;
            const unsigned long long ageMs = CyberpunkVR_DebugVrcamEyeAgeMs;
            if (ageMs != 0xFFFFFFFFull) {
                CVR_DIAGNOSTIC(CyberpunkVR_DebugEyeAgeCount.fetch_add(1, std::memory_order_relaxed));
                CVR_DIAGNOSTIC(CyberpunkVR_DebugEyeAgeSumMs.fetch_add(ageUs, std::memory_order_relaxed));
                unsigned long long prev =
                    CyberpunkVR_DebugEyeAgeMaxMs.load(std::memory_order_relaxed);
                while (ageUs > prev &&
                       !CyberpunkVR_DebugEyeAgeMaxMs.compare_exchange_weak(
                           prev, ageUs, std::memory_order_relaxed)) {
                }
                // Buckets in whole game frames (20 ms), so "one frame behind the other eye" reads as 1.
                int b = (int)(ageUs / 20000ull);
                if (b < 0) b = 0;
                if (b > 3) b = 3;
                CVR_DIAGNOSTIC(CyberpunkVR_DebugEyeAgeBuckets[b].fetch_add(1, std::memory_order_relaxed));
            } else {
                CVR_DIAGNOSTIC(CyberpunkVR_DebugEyeAgeNever.fetch_add(1, std::memory_order_relaxed));
            }
        }
        if (vrcamSrc) {
            if (EnsureVrcamEyeTexture(eyeW, eyeH, DXGI_FORMAT_R8G8B8A8_TYPELESS)) {
                if (!m_colorBlit) m_colorBlit = std::make_unique<ColorBlit>();
                if (m_colorBlit->EnsureInitialized(m_d3dDevice,
                                                   DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                                                   eyeW, eyeH)) {
                    // No barrier on vrcamSrc: a transition barrier asserts the resource's
                    // current state, and this one is not ours to assert about -- that lie is
                    // exactly what hung the device. A resource resting in COMMON is promoted
                    // to the read state implicitly for the access that needs it and decays
                    // back afterwards, asserting nothing. sync_stereo additionally creates it
                    // with ALLOW_SIMULTANEOUS_ACCESS so the overlap is defined.
                    // ROTATE FIRST, then write. The slot chosen here is the one the submit will
                    // look up by serial, so the producer never touches an image a consumer is
                    // still copying out of -- the whole point of the pool.
                    {
                        std::lock_guard<std::mutex> lock(m_presentMutex);
                        for(int step=1;step<=kVrcamEyeSlots;++step) {
                            const auto slot=(m_vrcamEyeSlot+step)%kVrcamEyeSlots;
                            if(m_vrcamEyePoolSerial[slot]==m_monoCapturedFrame.serial && m_monoCapturedFrame.serial)continue;
                            auto* candidate=m_vrcamEyePool[slot];
                            if(m_captureLeases.BeginWrite(reinterpret_cast<uintptr_t>(candidate))) {
                                m_vrcamEyeSlot=slot;writeResources[1]=reinterpret_cast<uintptr_t>(candidate);break;
                            }
                        }
                    }
                    if(!writeResources[1]) {
                        static const bool historyDiagnostic=[] {char flag[8]{};return GetEnvironmentVariableA("CPVR_FG_HISTORY_DIAG",flag,sizeof(flag))==1&&flag[0]=='1';}();
                        static uint64_t busySamples{};
                        if(historyDiagnostic && (++busySamples<=6||busySamples%180==0)) {
                            std::array<uint64_t,kVrcamEyeSlots> serials{};
                            std::array<uint32_t,kVrcamEyeSlots> readers{},writing{};
                            uint64_t published{};
                            {
                                std::lock_guard<std::mutex> lock(m_presentMutex);
                                published=m_monoCapturedFrame.serial;
                                for(int slot=0;slot<kVrcamEyeSlots;++slot) {
                                    const auto resource=reinterpret_cast<uintptr_t>(m_vrcamEyePool[slot]);
                                    serials[slot]=m_vrcamEyePoolSerial[slot];
                                    readers[slot]=m_captureLeases.Readers(resource);
                                    writing[slot]=unsigned(m_captureLeases.Writing(resource));
                                }
                            }
                            char slots[768]{};size_t used=0;
                            for(int slot=0;slot<kVrcamEyeSlots&&used<sizeof(slots);++slot) {
                                const int length=std::snprintf(slots+used,sizeof(slots)-used," slot%d=%llu/readers%u/writing%u",
                                    slot,(unsigned long long)serials[slot],readers[slot],writing[slot]);
                                if(length<0||size_t(length)>=sizeof(slots)-used)break;
                                used+=size_t(length);
                            }
                            Log("[VRCAM-POOL-BUSY] n=%llu capture=%llu published=%llu slots=%d%s\n",
                                (unsigned long long)busySamples,(unsigned long long)serial,(unsigned long long)published,kVrcamEyeSlots,slots);
                        }
                        m_captureCmdList->Close();snapshot->Release();return captureFailed(CaptureReason::NoSlot);
                    }
                    ID3D12Resource* const eyeSlotTex = m_vrcamEyePool[m_vrcamEyeSlot];
                    cvr::gpu::profile::Scope gpuScope(gpuProfile, cvr::gpu::profile::Stage::CaptureSecond);

                    D3D12_RESOURCE_BARRIER toRt{};
                    toRt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    toRt.Transition.pResource = eyeSlotTex;   // ours
                    toRt.Transition.StateBefore = fgtrace::CaptureColorReadState;
                    toRt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    toRt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    m_captureCmdList->ResourceBarrier(1, &toRt);

                    // ---- the HUD, for the eye the engine refuses to draw it for ----------
                    //
                    // When the HUD surface is available the composite REPLACES the plain blit:
                    // it is the engine's own HUD pass (PipelineState_576) ported shader-for-
                    // shader, and it reads the scene, so it produces the finished eye in one go
                    // -- curvature, the x2 gain, the mip glow and the chromatic aberration
                    // included. Without a HUD (loading, HUD off) it falls back to the blit.
                    ID3D12Resource* hud     = CyberpunkVR_GetHudTexture();
                    ID3D12Resource* hudBlur = hud ? CyberpunkVR_GetHudBlurTexture() : nullptr;
                    ID3D12Resource* hudExpo = hud ? CyberpunkVR_GetHudExposureBuffer() : nullptr;
                    ID3D12Resource* frameCb = hud ? CyberpunkVR_GetFrameConstantBuffer() : nullptr;
                    ID3D12Resource* hudCb   = hud ? CyberpunkVR_GetHudConstantBuffer() : nullptr;
                    CyberpunkVR_NoteHudCompositeInputs(hud, hudBlur, hudExpo, frameCb, hudCb);
                    // THE COMPOSITE CONSTANTS ARE OPTIONAL, and requiring them here is why the HUD
                    // took minutes to appear in the headset. They are found by fingerprinting the
                    // engine's upload traffic, which can take a long time and in a bad session
                    // never succeeds at all; meanwhile the shader already has a fallback -- it
                    // validates whatever is bound at b6 and uses its own captured constants when
                    // the block does not look like the real thing. The mirror path learned this
                    // and got the fallback; this one, the only path that reaches the headset,
                    // kept the guard. Bind the per-frame constants in their place and let the
                    // shader decide, exactly as the mirror does.
                    if (!hudCb) hudCb = frameCb;
                    if (hud && hudBlur && hudExpo && frameCb && hudCb) {
                        ColorBlit::HudParams hp = cvr::CyberpunkVR_GetHudParams();
                        // PUT THE HUD AT A DISTANCE INSTEAD OF AT INFINITY.
                        //
                        // The engine draws the HUD into MAIN at MAIN's pixel coordinates and we
                        // paste that same surface into this eye at the same coordinates, so its
                        // disparity is zero -- which is optical infinity. A tester on a Pimax put
                        // it exactly right: "both images too far to the sides to converge". Look at
                        // the world an arm's length away and every icon splits by the full
                        // vergence angle, far outside what the eyes can fuse.
                        //
                        // Only this eye can move: MAIN's HUD is the engine's own draw. So the
                        // whole disparity goes here, which also slides the HUD's apparent centre
                        // by half of it -- about 0.4 deg at the default distance, well under what
                        // reads as off-centre.
                        //
                        // shift = IPD / distance, in radians, converted to UV through the eye's
                        // own half-width and half-FOV, so it is right at any resolution or lens.
                        // Negative: an object nearer than infinity sits LEFT of its infinity
                        // position in the right eye.
                        const float hudDistM = CyberpunkVR_HudDistanceM;
                        if (hudDistM > 0.05f && m_eyeSwapchains[1].width > 0) {
                            XrFovf f{};
                            const float ipd = GetRuntimeIpd();
                            if (GetCurrentEyeFov(1, &f) && ipd > 0.03f && ipd < 0.10f) {
                                const float halfTan = 0.5f * (tanf(f.angleRight) - tanf(f.angleLeft));
                                if (halfTan > 0.01f) {
                                    // pixels per radian at the centre = halfWidth / tan(halfFov)
                                    const float uvPerRad = 0.5f / halfTan;
                                    hp.hudShiftU = -(ipd / hudDistM) * uvPerRad;
                                }
                            }
                        }
                        cvr::gpu::profile::Scope hudScope(gpuProfile, cvr::gpu::profile::Stage::SecondHud);
                        vrcamEyeCaptured = m_colorBlit->RecordHudComposite(
                            m_captureCmdList, vrcamSrc, hud, hudBlur, hudExpo, frameCb, hudCb,
                            eyeSlotTex, hp);
                    }
                    if (!vrcamEyeCaptured) {
                        cvr::gpu::profile::Scope blitScope(gpuProfile, cvr::gpu::profile::Stage::SecondBlit);
                        vrcamEyeCaptured = m_colorBlit->RecordBlit(m_captureCmdList, vrcamSrc,
                                                                   eyeSlotTex);
                    }
                    // The outline goes ON TOP of the finished eye, premultiplied, the way the
                    // engine's own composite adds it -- a separate pass rather than another input
                    // to the HUD shader, because it is a separate surface with its own lifetime
                    // and it is empty whenever nothing is being scanned. GetVisionTexture already
                    // returns null when the layer is stale, so the no-op case costs nothing.
                    if (vrcamEyeCaptured && CyberpunkVR_VisionToSecondEye) {
                        if (ID3D12Resource* vision = CyberpunkVR_GetVisionTexture()) {
                            if (m_colorBlit->RecordOverlay(m_captureCmdList, vision,
                                                           eyeSlotTex,
                                                           CyberpunkVR_VisionDebug,
                                                           CyberpunkVR_VisionFit != 0,
                                                           CyberpunkVR_VisionOffX,
                                                           CyberpunkVR_VisionOffY))
                                CVR_DIAGNOSTIC(++CyberpunkVR_DebugVisionOverlays);
                        }
                    }

                    if (vrcamEyeCaptured && CetOverlayRecordIntoTarget(
                            m_captureCmdList, eyeSlotTex, 0.0f)) {
                        if (++CyberpunkVR_DebugCetEyeComposites == 1)
                            Log("[cet-layer] VRCAM eye composite active.\n");
                    }

                    // AND THE OVERLAY ITSELF -- the F10 menu and the mouse cursor, which live in
                    // MAIN's backbuffer because that is where ImGui draws, and were therefore absent
                    // from this eye entirely. Its dedicated second-eye world list also carries the
                    // barrel dot, below the UI but above the HUD and scanner outline.
                    //
                    // The distance knob is the same idea as CyberpunkVR_HudDistanceM and defaults to
                    // the same 0: only this eye can move, so the whole disparity goes here, and the
                    // shift is the panel's angular offset (ipd / distance) converted to pixels through
                    // this eye's own half-width and half-FOV, so it holds at any resolution or lens.
                    if (vrcamEyeCaptured && CyberpunkVR_OverlaySecondEye) {
                        float shiftPx = 0.0f;
                        const float ovlDistM = CyberpunkVR_OverlaySecondEyeDistM;
                        if (ovlDistM > 0.05f && m_eyeSwapchains[1].width > 0) {
                            XrFovf f{};
                            const float ipd = GetRuntimeIpd();
                            if (GetCurrentEyeFov(1, &f) && ipd > 0.03f && ipd < 0.10f) {
                                const float halfTan = 0.5f * (tanf(f.angleRight) - tanf(f.angleLeft));
                                if (halfTan > 0.01f) {
                                    const float pxPerRad = (0.5f * static_cast<float>(eyeW)) / halfTan;
                                    shiftPx = (ipd / ovlDistM) * pxPerRad;
                                }
                            }
                        }
                        OverlayRecordIntoTarget(m_captureCmdList, eyeSlotTex, shiftPx);
                    }

                    // Why this is done here at all rather than in the engine: eye 0 is MAIN's
                    // backbuffer and already carries the HUD, while eye 1 is the VRCAM view,
                    // for which the composition group bails on a per-view gate we cannot
                    // satisfy without handing the RTT view a full output resource set -- two
                    // crashes established that. sync_stereo snapshots the HUD surface inside
                    // the engine's own list, so what is sampled here is a committed resource
                    // that cannot be aliased out from under us and rests in COMMON, promoted
                    // implicitly for the read: no barrier of ours on anything the engine owns.

                    D3D12_RESOURCE_BARRIER toSrc = toRt;
                    toSrc.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    toSrc.Transition.StateAfter = fgtrace::CaptureColorReadState;
                    m_captureCmdList->ResourceBarrier(1, &toSrc);
                } else {
                    static bool s_blitWarned = false;
                    if (!s_blitWarned) {
                        s_blitWarned = true;
                        Log("OpenXRManager: stereo ColorBlit init failed (%ux%u)"
                            " -- right eye stays on MAIN\n", eyeW, eyeH);
                    }
                }
            }
        }
    }

    // [DEPTH] Scene-depth snapshot for XR_KHR_composition_layer_depth, recorded on THIS
    // (capture) list so it is FIFO-ordered before the mono submit's depth copy on the
    // SAME queue -> no cross-queue Wait, no fence cycle.
    // CRITICAL: we do NOT transition the game depth (transitionGameDepth=false). D3D12
    // resource state is GLOBAL; issuing a barrier on the game's depth from our side
    // corrupts the state the game itself tracks and device-removes -> froze CP2077,
    // worst at intro / menu-load where the depth resource + state are transient (as the
    // user observed). Instead the resolve reads it as an SRV in whatever shader-readable
    // state the game already left it, so we only capture when:
    //   * mono depth is enabled,
    //   * a menu is NOT open,
    //   * the scene-depth has been the SAME resource (menus closed) for a warmup window
    //     -> skips the intro/menu-load transient depth entirely, and
    //   * it is shader-readable THIS frame.
    // Resource references must also survive recording and execution. A CPU-local
    // pointer alone does not keep a D3D12 copy's operands alive on the GPU.
    bool depthCaptured = false;
    if (!GetMonoDepthCapture()) m_depthStageCaptureAllowed.store(false, std::memory_order_release);
    if (GetMonoDepthCapture() != 0) {
        Microsoft::WRL::ComPtr<ID3D12Resource> depthLease;
        depthLease.Attach(OmoAcquireSceneDepthResource());
        ID3D12Resource* gameDepth = depthLease.Get();
        const UINT depthStateRaw = OmoGetSceneDepthState();
        const bool menuOpen = (GetMenuRectMode() != 0) || (GetMenuMode() != 0);
        const bool srvReadable = (depthStateRaw & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) != 0;
        static ID3D12Resource* s_depthGateRes = nullptr;
        static uint32_t s_depthGateStable = 0;
        if (gameDepth && gameDepth == s_depthGateRes && !menuOpen) {
            if (s_depthGateStable < 100000u) ++s_depthGateStable;
        } else {
            s_depthGateStable = 0;   // resource changed / menu open -> restart warmup
        }
        s_depthGateRes = gameDepth;
        const bool gateOk = gameDepth && !menuOpen && s_depthGateStable >= 60; // ~1s stable gameplay depth
        m_depthStageCaptureAllowed.store(gateOk && GetDepthSubmit() != 0, std::memory_order_release);
        bool snapOk = false;
        // Resolve from the STAGING copy taken at the readable barrier, not from the engine's
        // buffer. The stage is ours and sits in PIXEL_SHADER_RESOURCE, so the resolve no
        // longer depends on what state the engine happens to leave its depth in at Present --
        // which is what made the depth layer flicker.
        ID3D12Resource* resolveSrc = nullptr;
        Microsoft::WRL::ComPtr<ID3D12Resource> stageLease;
        D3D12_RESOURCE_STATES resolveState = D3D12_RESOURCE_STATE_COMMON;
        bool fromStage = false;
        {
            std::lock_guard<std::mutex> lock(m_depthStageMutex);
            if (m_depthStage && m_depthStageSerial == serial) {
                stageLease = m_depthStage;
                resolveSrc = stageLease.Get();
                resolveState = D3D12_RESOURCE_STATE_COPY_SOURCE;
                fromStage = true;
            }
        }
        if (!fromStage && srvReadable) {          // fallback: the old direct read
            resolveSrc = gameDepth;
            resolveState = static_cast<D3D12_RESOURCE_STATES>(depthStateRaw);
        }
        if (gateOk && resolveSrc && (snapOk = EnsureDepthSnapshot(gameDepth))) {
            cvr::gpu::profile::Scope depthScope(gpuProfile, cvr::gpu::profile::Stage::CaptureDepth);
            depthCaptured = RecordDepthCapture(m_captureCmdList, resolveSrc, resolveState,
                                               /*transitionGameDepth=*/false);
        }
        // Say WHICH condition is holding depth back. Four separate gates decide this and the
        // submit line only ever reported the outcome (depth=0), which is why it stayed a
        // guess for so long.
        if ((serial % 300) == 1) {
            Log("Depth gate: res=%p stable=%u menu=%d srvReadable=%d(state=0x%X) stage=%d "
                "snapshot=%d captured=%d\n",
                gameDepth, s_depthGateStable, menuOpen ? 1 : 0, srvReadable ? 1 : 0,
                depthStateRaw, fromStage ? 1 : 0, snapOk ? 1 : 0, depthCaptured ? 1 : 0);
        }
    }

    m_captureCmdList->Close();
    ID3D12CommandList* cmdLists[] = {m_captureCmdList};
    XrPosef capturedPoses[2]={poses[0],poses[1]};
    uint64_t capturedPoseIds[2]{},capturedImageGenerations[2]{};
    std::shared_ptr<const cvr::framegen::Inputs> capturedInputs[2];
    std::optional<cvr::framegen::FrameProjectionMetadata> capturedProjections[2];
    cvr::framegen::CaptureCompletion captureCompletion;
    std::array<fgtrace::ProducerTraceRecord,2> captureProvenance;
    fgtrace::NativeFrameStamp nativeStamp;
    bool publishMainSourceOnly=false;
    uint32_t publishedMainEye=0;
    // This is the author's final SDR capture path. Its existing Framegen
    // generator explicitly declares FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB.
    // Keep that pipeline contract separate from the texture's UNORM view.
    if(sourceDesc.Format==DXGI_FORMAT_R8G8B8A8_UNORM || sourceDesc.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS ||
       sourceDesc.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)nativeStamp.transfer=fgtrace::CaptureTransfer::SrgbEncoded;
    {
        // Select labels at the actual queue submission, not when a producer
        // merely recorded commands. The same-queue copy now sits between the
        // labelled writes and any subsequent replacement of the source images.
        std::lock_guard submissionLock(cvr::camera::ImageSubmissionMutex());
        const auto mainImage=cvr::camera::ReadImagePose(backBuffer);
        if(nativeColor && (!cvr::render::ValidateNativeColor(*nativeColor,m_d3dQueue,mainImage) ||
           nativeColor->resource.Get()!=backBuffer)) {
            snapshot->Release();return captureFailed(CaptureReason::MainReceiptRejected);
        }
        const auto vrcamImage=cvr::camera::ReadImagePose(vrcamCaptureSource);
        const bool mainSourceRequested=cvr::camera::MainEyeSourceRequested();
        if(!mainSourceRequested){static uint64_t pairedCaptureTicks{};
            if(++pairedCaptureTicks<=6||pairedCaptureTicks%240==0)Log("[PAIRED-CAPTURE] serial=%llu main=%d vrcam=%d captured=%d rendered=%llux%u mainSize=%llux%u\n",
                serial,int(bool(mainImage)),int(bool(vrcamImage)),int(vrcamEyeCaptured),vrcamRenderedDesc.Width,vrcamRenderedDesc.Height,sourceDesc.Width,sourceDesc.Height);}
        const auto mainSourceReceipt=mainImage ? mainImage.pose.mainEyeSource : cvr::framegen::MainEyeSourceReceipt{};
        const bool mainSourceValid=mainSourceReceipt && cvr::camera::ValidateMainEyeSourceReceipt(mainSourceReceipt);
        // Preserve the raw image-label ID even when validation rejects it;
        // receiptFlags distinguishes that observation from an accepted source.
        captureDiagnostic.sourceId=mainSourceReceipt.sourceId;
        captureDiagnostic.receiptFlags=1u|(mainSourceRequested?2u:0u)|(mainSourceReceipt?4u:0u)|(mainSourceValid?8u:0u);
        const auto mainCaptureMode=cvr::framegen::PlanMainEyeCapture(mainSourceRequested,bool(mainSourceReceipt),mainSourceValid);
        if(mainCaptureMode==cvr::framegen::MainEyeCaptureMode::Reject) {
            // Nothing recorded on this capture list has been submitted yet.
            // WriteRelease returns both reserved slots; no stale epoch's raw
            // color/matrices/fence are published as a new MAIN source.
            snapshot->Release();return captureFailed(CaptureReason::MainReceiptRejected);
        }
        publishMainSourceOnly=mainCaptureMode==cvr::framegen::MainEyeCaptureMode::Source;
        if(publishMainSourceOnly)publishedMainEye=static_cast<uint32_t>(mainSourceReceipt.physicalEye);
        const uint32_t vrcamEye=CyberpunkVR_MainIsRightEye ? 0u : 1u;
        for(uint32_t eye=0;eye<2;++eye) {
            // A MAIN-only source labels precisely its applied physical eye.
            // Do not mint a second eye/source from the same MAIN texture, and
            // do not trust a current global eye swap for an in-flight image.
            const bool separate=!mainSourceRequested && vrcamEyeCaptured && eye==vrcamEye;
            const auto& source=separate ? vrcamImage : mainImage;
            const uint32_t view=separate ? 2u : 1u;
            auto& trace=captureProvenance[eye];trace.stage=fgtrace::ProducerStage::ColorPublished;
            trace.reason=fgtrace::ProducerReason::CapturePoseUnverified;
            trace.engineView=separate ? fgtrace::EngineView::Vrcam : fgtrace::EngineView::Main;
            trace.logicalEye=eye;trace.flags=fgtrace::EngineViewKnown|fgtrace::LogicalEyeKnown;
            trace.sourceId=serial;if(serial)trace.flags|=fgtrace::SourceIdKnown;
            if(mainSourceRequested && (!mainSourceValid || eye!=static_cast<uint32_t>(mainSourceReceipt.physicalEye)))continue;
            if(source && source.pose.mainEyeSource &&
               (!publishMainSourceOnly || !cvr::camera::ValidateMainEyeSourceReceipt(source.pose.mainEyeSource) ||
                eye!=static_cast<uint32_t>(source.pose.mainEyeSource.physicalEye)))continue;
            if(source) {
                trace.poseId=source.pose.poseId;trace.poseWriteId=source.pose.writeId;
                trace.identityView=source.pose.view;trace.spaceEpoch=source.pose.head.originSerial;
                trace.nativeFrame=source.pose.renderFrameId;
                if(trace.poseId)trace.flags|=fgtrace::PoseIdKnown;
                if(trace.poseWriteId)trace.flags|=fgtrace::PoseWriteIdKnown;
                if(trace.spaceEpoch)trace.flags|=fgtrace::SpaceEpochKnown;
                if(source.pose.hasRenderFrameId)trace.flags|=fgtrace::NativeFrameKnown;
                if(source.pose.head.valid)trace.flags|=fgtrace::HeadPoseValid;
            }
            if(CyberpunkVR_BindPoseToImage && !ExternalPoseResetPending() && source && source.pose && source.pose.view==view &&
               source.queue==reinterpret_cast<uintptr_t>(m_d3dQueue) &&
               source.pose.head.originSerial==GetTrackingOriginSerial()) {
                capturedPoses[eye]=cvr::camera::RebaseImageEyePose(headCenters[eye],poses[eye],source.pose.localHead);
                capturedPoseIds[eye]=source.pose.poseId;
                capturedImageGenerations[eye]=source.generation;
                auto& stamp=nativeStamp.eyes[eye];stamp.sourceSerial=serial;
                stamp.poseId=source.pose.poseId;stamp.poseWriteId=source.pose.writeId;stamp.spaceEpoch=source.pose.head.originSerial;
                stamp.engineView=view;stamp.nativeFrame=source.pose.renderFrameId;stamp.nativeFrameKnown=source.pose.hasRenderFrameId;
                const auto renderedDesc=separate?vrcamRenderedDesc:sourceDesc;
                stamp.renderedWidth=uint32_t(renderedDesc.Width);stamp.renderedHeight=renderedDesc.Height;
                stamp.clockGeneration=source.pose.clockGeneration;stamp.cameraApplicationQpc=source.pose.cameraApplicationQpc;
                stamp.contentTimeNs=source.pose.contentTimeNs;stamp.posePredictionNs=source.pose.posePredictionNs;
                stamp.mainEyeSource=source.pose.mainEyeSource;
                stamp.referenceOptics=source.pose.head.referenceOptics;
                const auto& imageHead=source.pose.localHead;
                stamp.capturedHead={imageHead.orientation.x,imageHead.orientation.y,
                    imageHead.orientation.z,imageHead.orientation.w,
                    imageHead.position.x,imageHead.position.y,imageHead.position.z};
                stamp.capturedHeadValid=fgtrace::CapturedHeadTrusted(source.pose.externalView,
                    publishMainSourceOnly&&mainSourceValid,source.pose.mainEyeSource,
                    source.pose.head.originSerial,source.pose.head.locatedTime);
                trace.flags|=fgtrace::PoseIdentityValid;trace.reason=fgtrace::ProducerReason::ColorScheduled;
                const bool guideCaptureRequested=cvr::framegen::CaptureInputsEnabled();
                if(guideCaptureRequested && source.pose.hasRenderFrameId)
                    capturedInputs[eye]=cvr::framegen::AcquireInputs(view,source.pose.renderFrameId,
                        source.pose.poseId,source.pose.head.originSerial,m_d3dQueue);
                if(capturedInputs[eye])capturedProjections[eye]=cvr::framegen::SnapshotFrameProjection(*capturedInputs[eye]);
                else if(source.pose.hasRenderFrameId)capturedProjections[eye]=cvr::framegen::AcquireFrameProjection(
                    view,source.pose.renderFrameId,source.pose.poseId,source.pose.head.originSerial,m_d3dQueue);
                if(!capturedProjections[eye]&&source.pose.hasRenderFrameId) {
                    cvr::framegen::SubmittedProjectionReason projectionReason;
                    capturedProjections[eye]=cvr::framegen::AcquireSubmittedImageProjection(source,&projectionReason);
                    if(cvr::framegen::OwnSelectionWitnessEnabled()) {
                        static std::atomic<uint64_t> calls{},nextLog{};const auto count=++calls;
                        const auto now=GetTickCount64();auto due=nextLog.load(std::memory_order_relaxed);
                        if(count<=4||(now>=due&&nextLog.compare_exchange_strong(due,now+1000,std::memory_order_relaxed))) {
                            const auto& match=capturedProjections[eye];
                            Log("[OWN-PROJECTION-SOURCE] serial=%llu eye=%u view=%u frame=%u pose=%llu write=%llu origin=%llu clock=%llu imageGeneration=%llu status=%u matched=%u matchedFrame=%u matchedWrite=%llu constantsVersion=%llu\n",
                                serial,eye,view,source.pose.renderFrameId,source.pose.poseId,source.pose.writeId,
                                source.pose.head.originSerial,source.pose.clockGeneration,source.generation,unsigned(projectionReason),unsigned(bool(match)),
                                match?match->frameId:0,match?match->poseWriteId:0,match?uint64_t(match->constants.version):0);
                        }
                    }
                }
                if(capturedInputs[eye])trace.flags|=fgtrace::GuidesAvailable;
                else if(guideCaptureRequested)trace.reason=fgtrace::ProducerReason::CaptureGuidesMissing;
                if(capturedInputs[eye])captureDiagnostic.guidesMask|=uint64_t{1}<<eye;
                else if(guideCaptureRequested)CyberpunkVR_AerCaptureDebug.Count(CaptureReason::GuidesMissing);
                CVR_DIAGNOSTIC(CyberpunkVR_PoseIdCaptured[view-1].fetch_add(1,std::memory_order_relaxed));
                CVR_DIAGNOSTIC(CyberpunkVR_PoseIdLastCapture[eye].store(source.pose.poseId,std::memory_order_relaxed));
                CVR_DIAGNOSTIC(CyberpunkVR_PoseIdImageGeneration[eye].store(source.generation,std::memory_order_relaxed));
            } else {
                // A populated source can still fail the final queue/origin/reset
                // checks. Keep this bounded observation separate from admission:
                // it must never substitute a current pose for the rendered one.
                if(source && source.pose) {
                    static std::atomic<uint64_t> labelledPoseMisses{};
                    const auto miss=++labelledPoseMisses;
                    if(miss<=12 || miss%600==0) {
                        Log("[PAIRED-POSE-MISS] serial=%llu eye=%u bind=%d reset=%d view=%u/%u origin=%llu/%llu queue=%p/%p pose=%llu write=%llu native=%u known=%d\n",
                            serial,eye,CyberpunkVR_BindPoseToImage,int(ExternalPoseResetPending()),source.pose.view,view,
                            source.pose.head.originSerial,GetTrackingOriginSerial(),reinterpret_cast<void*>(source.queue),static_cast<void*>(m_d3dQueue),
                            source.pose.poseId,source.pose.writeId,source.pose.renderFrameId,int(source.pose.hasRenderFrameId));
                        // The SL API transfers a reference even for an unchanged
                        // interface. Inspect aliases without changing admission.
                        void* sourceNative{};void* captureNative{};
                        const auto sourceResult=QueryNativeQueueForDiagnostic(reinterpret_cast<void*>(source.queue),&sourceNative);
                        const auto captureResult=QueryNativeQueueForDiagnostic(m_d3dQueue,&captureNative);
                        Microsoft::WRL::ComPtr<IUnknown> sourceOwner,captureOwner;
                        if(sourceResult==0&&sourceNative)sourceOwner.Attach(static_cast<IUnknown*>(sourceNative));
                        if(captureResult==0&&captureNative)captureOwner.Attach(static_cast<IUnknown*>(captureNative));
                        Log("[PAIRED-QUEUE-IDENTITY] serial=%llu source=%p result=%d native=%p capture=%p result=%d native=%p\n",
                            serial,reinterpret_cast<void*>(source.queue),sourceResult,sourceNative,
                            static_cast<void*>(m_d3dQueue),captureResult,captureNative);
                    }
                }
                CVR_DIAGNOSTIC(CyberpunkVR_PoseIdCaptureMiss[view-1].fetch_add(1,std::memory_order_relaxed));
                CVR_DIAGNOSTIC(CyberpunkVR_PoseIdLastCapture[eye].store(0,std::memory_order_relaxed));
                CVR_DIAGNOSTIC(CyberpunkVR_PoseIdImageGeneration[eye].store(0,std::memory_order_relaxed));
            }
        }
        if(publishMainSourceOnly && !nativeStamp.eyes[publishedMainEye].mainEyeSource) {
            // The ordinary image-pose/queue/origin checks below must have
            // produced the receipt too; a valid latch alone is insufficient.
            snapshot->Release();return captureFailed(CaptureReason::NativeStampMissing);
        }
        if(publishMainSourceOnly)captureDiagnostic.receiptFlags|=16u;
        m_d3dQueue->ExecuteCommandLists(1,cmdLists);
        ++m_captureFenceValue;
        const HRESULT captureSignal=m_d3dQueue->Signal(m_captureFence,m_captureFenceValue);
        m_captureContextFences.Executed(m_captureAllocatorIndex,m_captureFenceValue,SUCCEEDED(captureSignal));
        m_mainPoolProducer.Executed(m_captureFence,m_captureFenceValue,SUCCEEDED(captureSignal));
        captureDiagnostic.captureFenceValue=m_captureFenceValue;
        captureDiagnostic.producerUnknown=m_mainPoolProducer.unknown?1:0;
        if(FAILED(captureSignal))CyberpunkVR_AerCaptureDebug.Count(CaptureReason::SignalFailed);
        LARGE_INTEGER captureQpc{};
        if(SUCCEEDED(captureSignal))QueryPerformanceCounter(&captureQpc);
        static const int64_t captureQpcFrequency=[] {
            LARGE_INTEGER frequency{};return QueryPerformanceFrequency(&frequency) ? frequency.QuadPart : int64_t{};
        }();
        captureCompletion=cvr::framegen::CaptureCompletion::AfterSignal(
            m_captureFence,m_captureFenceValue,captureSignal,captureQpc,captureQpcFrequency);
        if(nativeColor && (serial%300==1 || (mainSourceReceipt.sourceId>=800 && mainSourceReceipt.sourceId<=803)))
            Log("[NATIVE-COLOR-SOURCE] serial=%llu source=%lld eye=%u native=%p window=%p size=%llux%u state=%u fence=%llu signal=%08X\n",
                static_cast<unsigned long long>(serial),static_cast<long long>(mainSourceReceipt.sourceId),
                unsigned(mainSourceReceipt.physicalEye),backBuffer,reinterpret_cast<void*>(nativeColor->window),
                static_cast<unsigned long long>(sourceDesc.Width),sourceDesc.Height,unsigned(sourceState),
                static_cast<unsigned long long>(m_captureFenceValue),unsigned(captureSignal));
        if (SUCCEEDED(captureSignal))
            cvr::gpu::profile::Submitted(gpuProfile, m_d3dQueue, m_captureFence, m_captureFenceValue);
    }

    if(!publishMainSourceOnly&&vrcamEyeCaptured){
        if(const auto time=SampleCameraApplicationTime(GetTrackingOriginSerial())){
            nativeStamp.renderPublicationTimeNs=time->xrTime;nativeStamp.publicationClockGeneration=time->clockGeneration;}
    }
    {
        std::lock_guard<std::mutex> lock(m_presentMutex);
        // Publish the buffer we just filled as the current frame. This is the hand-off:
        // everything the consumer needs -- image, poses, fovs, serial -- becomes visible in
        // one locked step, and the buffer it was reading before stays untouched.
        m_monoCapturedFrame.texture = snapshot;
        if (m_monoCapturedFrame.texture == snapshot) {
            m_monoCapturedFrame.serial = serial;
            m_monoCapturedFrame.colorCompletion = captureCompletion;
        nativeStamp.resourceEpoch=m_captureResourceEpoch;
            m_monoCapturedFrame.nativeStamp=nativeStamp;
            m_monoCapturedFrame.captureMs = XrDiagNowMs();
            CVR_DIAGNOSTIC(CyberpunkVR_DebugCapOk.fetch_add(1, std::memory_order_relaxed));
            for (int eye = 0; eye < 2; ++eye) {
                m_monoCapturedFrame.poses[eye] = capturedPoses[eye];
                m_monoCapturedFrame.poseIds[eye]=capturedPoseIds[eye];
                m_monoCapturedFrame.imageGenerations[eye]=capturedImageGenerations[eye];
                m_monoCapturedFrame.framegenInputs[eye]=capturedInputs[eye];
                m_monoCapturedFrame.projections[eye]=capturedProjections[eye];
                m_monoCapturedFrame.fovs[eye] = fovs[eye];
                m_monoCapturedFrame.hasView[eye] = hasView[eye] &&
                    (!publishMainSourceOnly || static_cast<uint32_t>(eye)==publishedMainEye);
            }
            // Catalogue entries retain CPU camera/identity metadata, never a
            // four-slot Inputs Record. Ordered entries reserve their physical
            // cell until the completed publication transfers to a ReadLease.
            if(publishMainSourceOnly && captureCompletion.HasScheduledCopySource() &&
               m_monoSubmitEnabled.load(std::memory_order_relaxed)) {
                const auto eye=publishedMainEye;
                cvr::framegen::Frame publication;
                publication.color[eye]=snapshot;publication.engineView[eye]=1;
                publication.pose[eye]=capturedPoses[eye];publication.fov[eye]=fovs[eye];
                publication.identity.serial=serial;
                publication.identity.origin=nativeStamp.eyes[eye].spaceEpoch;
                publication.identity.pose[eye]=capturedPoseIds[eye];
                publication.identity.nativeFrame=nativeStamp.eyes[eye].nativeFrame;
                publication.identity.capturedMs=m_monoCapturedFrame.captureMs;
                publication.nativeStamp=nativeStamp;publication.colorCompletion=captureCompletion;
                publication.projection[eye]=capturedProjections[eye];
                if(publication.projection[eye])m_mainCaptureCatalogue.Publish(catalogueWrite,nativeStamp.eyes[eye].mainEyeSource,
                    serial,std::move(publication),capturedImageGenerations[eye]);
            }
            // AER reads the immutable projection snapshots above. Full NVIDIA
            // guides must be reacquired/validated at actual generation input;
            // keeping this latest alias would unnecessarily pin a guide slot.
            if(publishMainSourceOnly)for(auto& input:m_monoCapturedFrame.framegenInputs)input.reset();
            SetD3DNamef(m_monoCapturedFrame.texture, L"OpenXR_mono_snapshot_serial%llu",
                static_cast<unsigned long long>(serial));
            if (depthCaptured) {
                m_depthSnapshotSerial = serial;
            }
            // Stamp the right eye with the SAME serial as the colour it belongs to. The submit
            // requires the match, so a frame where the VRCAM blit did not happen simply falls
            // back to MAIN in that eye instead of pairing mismatched images.
            // PER-SLOT, so a submit can ask for the frame it is actually carrying rather than the
            // newest one. m_vrcamEyeSerial stays as the newest, for the log lines that report it.
            m_vrcamEyeSerial = vrcamEyeCaptured ? serial : 0;
            if (vrcamEyeCaptured) {
                m_vrcamEyePoolSerial[m_vrcamEyeSlot] = serial;
                const auto eye=CyberpunkVR_MainIsRightEye ? 0u : 1u;
                m_vrcamEyeLabels[m_vrcamEyeSlot]={capturedPoses[eye],fovs[eye],capturedPoseIds[eye],capturedImageGenerations[eye],true};
            }
        }
        for(auto& resource:writeResources) {
            if(resource)m_captureLeases.EndWrite(resource);
            resource=0;
        }
        // Both eye labels and write reservations are now committed together.
        // Emit before releasing the publication lock so a consumer reservation
        // cannot appear ahead of its source publication in the trace.
        for(auto& trace:captureProvenance) {
            ApplyProducerCaptureCompletion(trace,captureCompletion);
            const auto& stamp=nativeStamp.eyes[trace.logicalEye];
            trace.contentTimeNs=stamp.contentTimeNs;trace.poseTimeNs=stamp.posePredictionNs;
            if(stamp.clockGeneration && stamp.contentTimeNs>0)trace.flags|=fgtrace::ContentTimeValid;
            if(stamp.posePredictionNs>0)trace.flags|=fgtrace::PoseTimeValid;
            trace.resourceEpoch=nativeStamp.resourceEpoch;
            if(trace.resourceEpoch)trace.flags|=fgtrace::ResourceEpochKnown;
            if(!captureCompletion.HasScheduledCopySource())trace.reason=fgtrace::ProducerReason::ColorSignalFailed;
            fgtrace::RecordProducerTrace(trace);
        }
    }
    if(cvr::framegen::NativeFgStereoRequested()&&!cvr::framegen::GetSettings().aerV2&&!cvr::framegen::CaptureInputsEnabled())
        cvr::framegen::ClearNativeFgPairedSource();
    if(cvr::framegen::NativeFgStereoRequested()&&cvr::framegen::CaptureInputsEnabled()&&!cvr::framegen::GetSettings().aerV2&&!m_stopFrameThread.load(std::memory_order_acquire)){
        const auto scene=cvr::framegen::ReadSourceFrameSystemStatus().scene;
        auto source=std::make_shared<cvr::framegen::NativeFgPairedSource>();
        source->gameWorldEpoch=scene.valid&&!scene.paused?scene.worldEpoch:0;
        source->mainEye=CyberpunkVR_MainIsRightEye?1u:0u;
        auto& frame=source->frame;frame.nativeStamp=nativeStamp;frame.colorCompletion=captureCompletion;
        frame.identity.serial=serial;frame.identity.origin=GetTrackingOriginSerial();frame.identity.stereo=true;
        frame.identity.capturedMs=XrDiagNowMs();frame.identity.nativeFrame=nativeStamp.eyes[0].nativeFrame;
        frame.identity.inputs=bool(capturedInputs[0])&&bool(capturedInputs[1]);
        auto lease=std::make_shared<cvr::framegen::ReadLease>();lease->owner=this;
        lease->release=[](void* owner,ID3D12Resource* resource,uint64_t fence){static_cast<OpenXRManager*>(owner)->ReleaseCapturedRead(resource,fence);};
        frame.lease=lease;
        {std::lock_guard lock(m_presentMutex);
            if(m_monoCapturedFrame.serial==serial&&vrcamEyeCaptured&&m_vrcamEyePoolSerial[m_vrcamEyeSlot]==serial){
                frame.color[source->mainEye]=snapshot;frame.color[source->mainEye^1]=m_vrcamEyePool[m_vrcamEyeSlot];
                for(unsigned eye=0;eye<2;++eye)if(frame.color[eye]&&m_captureLeases.Pin(uintptr_t(frame.color[eye]))){
                    lease->resources[eye]=frame.color[eye];frame.color[eye]->AddRef();}
            }
        }
        for(unsigned eye=0;eye<2;++eye){frame.engineView[eye]=nativeStamp.eyes[eye].engineView;
            frame.pose[eye]=capturedPoses[eye];frame.fov[eye]=fovs[eye];frame.identity.pose[eye]=capturedPoseIds[eye];frame.inputs[eye]=capturedInputs[eye];
            frame.projection[eye]=capturedProjections[eye];
            if(frame.projection[eye]){
                frame.identity.reset|=frame.projection[eye]->reset;
                if(frame.color[eye]){const auto d=frame.color[eye]->GetDesc();cvr::framegen::AerRealGeometry geometry;
                    geometry.width=uint32_t(d.Width);geometry.height=d.Height;
                    if(cvr::framegen::AerRealPresenter::ResolveCapturedGeometry(frame.projection[eye]->constants,nativeStamp.eyes[eye].referenceOptics,eye,geometry))
                        frame.fov[eye]=geometry.renderedFov;
                    else frame.identity.inputs=false;}
            }
        }
        if(cvr::framegen::ValidNativeFgPairedSource(*source))cvr::framegen::PublishNativeFgPairedSource(std::move(source));
        else cvr::framegen::ClearNativeFgPairedSource();
    }
    if (m_monoPresentEvent) {
        SetEvent(m_monoPresentEvent);
    }

    if(cvr::framegen::MetricsEnabled())cvr::framegen::OnRealFrame(serial,cvr::framegen::NowMs());

    snapshot->Release();
    CyberpunkVR_AerCaptureDebug.Record(CaptureReason::Published,captureDiagnostic);
    if (g_verboseLog && (serial % 300) == 1) {
        Log("OpenXRManager: Mono frame captured. serial=%llu\n",
            static_cast<unsigned long long>(serial));
    }
    return true;
}

bool OpenXRManager::EnsureMonoSubmitResources() {
    if (!m_monoSubmitEnabled.load(std::memory_order_relaxed)) {
        return false;
    }
    if (!m_d3dDevice || !m_d3dQueue || m_session == XR_NULL_HANDLE) {
        return false;
    }
    // No CPU wait. Pending reads are collected, while an unknown Signal/device
    // removal stops further recording/execution instead of growing retirement.
    if(!PollSubmitResources(false))return false;

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    cvr::ppd::ReferenceOptics sourceOptics;
    const bool referenceOutput=cvr::framegen::GetSettings().aerV2 || CyberpunkVR_StereoSubmit;
    {
        std::lock_guard<std::mutex> lock(m_presentMutex);
        width = m_lastPresentedWidth;
        height = m_lastPresentedHeight;
        format = m_lastPresentedFormat;
        if(referenceOutput&&m_monoCapturedFrame.texture){
            const auto desc=m_monoCapturedFrame.texture->GetDesc();
            if(desc.Width==width&&desc.Height==height&&m_monoCapturedFrame.nativeStamp.resourceEpoch==m_captureResourceEpoch)
                for(const auto& stamp:m_monoCapturedFrame.nativeStamp.eyes)
                    if(stamp.nativeFrameKnown&&stamp.referenceOptics.valid)sourceOptics=stamp.referenceOptics;
        }
    }

    if (width == 0 || height == 0 || format == 0) {
        return false;
    }
    std::array<uint32_t,2> eyeWidths{width,width},eyeHeights{height,height};
    if(referenceOutput&&sourceOptics.valid){
        const auto fit=cvr::ppd::CalibrateReferenceOptics(width,height,sourceOptics);
        for(unsigned eye=0;eye<2;++eye){
            const auto rect=cvr::ppd::ReferenceEyeCrop(width,height,fit,sourceOptics.applied[eye]);
            if(rect[2]<=rect[0]||rect[3]<=rect[1])return false;
            eyeWidths[eye]=uint32_t(rect[2]-rect[0]);eyeHeights[eye]=uint32_t(rect[3]-rect[1]);
        }
    }

    uint32_t runtimeFormatCount = 0;
    XrResult xrRes = xrEnumerateSwapchainFormats(m_session, 0, &runtimeFormatCount, nullptr);
    if (XR_FAILED(xrRes) || runtimeFormatCount == 0) {
        Log("OpenXRManager: xrEnumerateSwapchainFormats count failed (res=%d count=%u)\n", xrRes, runtimeFormatCount);
        return false;
    }

    std::vector<int64_t> runtimeFormats(runtimeFormatCount);
    xrRes = xrEnumerateSwapchainFormats(m_session, runtimeFormatCount, &runtimeFormatCount, runtimeFormats.data());
    if (XR_FAILED(xrRes) || runtimeFormatCount == 0) {
        Log("OpenXRManager: xrEnumerateSwapchainFormats list failed (res=%d count=%u)\n", xrRes, runtimeFormatCount);
        return false;
    }

    const auto formatSelection = cvr::openxr::PickSwapchainFormat(
        runtimeFormats, static_cast<DXGI_FORMAT>(format));
    if (formatSelection.format == DXGI_FORMAT_UNKNOWN) {
        Log("OpenXRManager: no compatible sRGB or UNORM swapchain format for source=%u\n", format);
        return false;
    }
    const int64_t selectedFormat = static_cast<int64_t>(formatSelection.format);

    // Pick a runtime-supported depth format ONLY AFTER the game's scene depth resource
    // has been pinned. This remains intentionally conservative: only the R32-family
    // depth path is considered stable. The 64-bit R32G8X24 typeless family caused
    // repeated GPU removal during snapshot/submission experiments, so depth is kept
    // disabled there to preserve a working Mono baseline.
    Microsoft::WRL::ComPtr<ID3D12Resource> pinnedDepthLease;
    pinnedDepthLease.Attach(OmoAcquireSceneDepthResource());
    ID3D12Resource* pinnedDepth = pinnedDepthLease.Get();
    const DXGI_FORMAT pinnedDepthFormat = pinnedDepth ? pinnedDepth->GetDesc().Format : DXGI_FORMAT_UNKNOWN;
    int64_t selectedDepthFormat = 0;
    // CP2077 mono-only mode hangs at start-up when a depth swapchain is created
    // but never populated (VirtualDesktopXR stalls waiting on the depth layer).
    // Skip depth swapchain creation unless the user explicitly opted in to mono depth
    // capture.
    const bool depthWanted = GetDepthSubmit() != 0 && GetMonoDepthCapture() != 0;
    if (depthWanted && m_depthLayerSupported && pinnedDepth) {
        // Only R32-family (D32_FLOAT 32bpp) is supported for now. CP2077's
        // R32-family (32bpp) accepted directly. R32G8X24-family (64bpp) accepted
        // too: the depth-plane resolve shader (DepthResolve) converts plane 0
        // of the typeless source into the same 32bpp D32_FLOAT snapshot used
        // by the 32bpp path, so the depth swapchain is always D32_FLOAT
        // regardless of game depth format.
        const bool gameIs32bpp =
            pinnedDepthFormat == DXGI_FORMAT_R32_TYPELESS ||
            pinnedDepthFormat == DXGI_FORMAT_D32_FLOAT ||
            pinnedDepthFormat == DXGI_FORMAT_R32_FLOAT;
        const bool gameIs64bpp =
            pinnedDepthFormat == DXGI_FORMAT_R32G8X24_TYPELESS ||
            pinnedDepthFormat == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
            pinnedDepthFormat == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS ||
            pinnedDepthFormat == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
        if (gameIs32bpp || gameIs64bpp) {
            for (int64_t rf : runtimeFormats) {
                if (rf == static_cast<int64_t>(DXGI_FORMAT_D32_FLOAT)) {
                    selectedDepthFormat = rf;
                    break;
                }
            }
        }
        if (selectedDepthFormat == 0) {
            // Log once per format transition only — EnsureMonoSubmitResources runs
            // every frame and would otherwise flood the log with thousands of
            // duplicate lines.
            static DXGI_FORMAT s_lastLoggedRejected = DXGI_FORMAT_UNKNOWN;
            if (s_lastLoggedRejected != pinnedDepthFormat) {
                s_lastLoggedRejected = pinnedDepthFormat;
                Log("OpenXRManager: depth layer disabled (gameFmt=%u not depth-resolvable, or runtime lacks D32_FLOAT)\n",
                    static_cast<unsigned>(pinnedDepthFormat));
            }
            m_depthLayerSupported = false;
        } else {
            static DXGI_FORMAT s_lastLoggedSelected = DXGI_FORMAT_UNKNOWN;
            static int64_t s_lastLoggedDepthSel = 0;
            if (s_lastLoggedSelected != pinnedDepthFormat ||
                s_lastLoggedDepthSel != selectedDepthFormat) {
                s_lastLoggedSelected = pinnedDepthFormat;
                s_lastLoggedDepthSel = selectedDepthFormat;
                Log("OpenXRManager: depth format gameFmt=%u selected=%lld\n",
                    static_cast<unsigned>(pinnedDepthFormat),
                    selectedDepthFormat);
            }
        }
    }
    const bool wantDepthSwapchains = m_depthLayerSupported && selectedDepthFormat != 0;
    if (wantDepthSwapchains && selectedDepthFormat != m_depthSwapchainFormat) {
        Log("OpenXRManager: depth swapchain format selected=%lld (pinnedDepthFmt=%u)\n",
            selectedDepthFormat, static_cast<unsigned>(pinnedDepthFormat));
    }

    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(m_instance, m_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
    m_viewConfigViews.resize(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(m_instance, m_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, m_viewConfigViews.data());
    m_views.resize(viewCount, {XR_TYPE_VIEW});

    const bool haveDepthSwapchains = !wantDepthSwapchains ||
        (!m_eyeSwapchains.empty() &&
         m_eyeSwapchains[0].depthHandle != XR_NULL_HANDLE &&
         (m_eyeSwapchains.size() < 2 || m_eyeSwapchains[1].depthHandle != XR_NULL_HANDLE));
    const bool eyeSizesMatch=m_eyeSwapchains.size()==2&&
        m_eyeSwapchains[0].width==int32_t(eyeWidths[0])&&m_eyeSwapchains[0].height==int32_t(eyeHeights[0])&&
        m_eyeSwapchains[1].width==int32_t(eyeWidths[1])&&m_eyeSwapchains[1].height==int32_t(eyeHeights[1]);
    const bool colorResourcesReady = eyeSizesMatch &&
        m_eyeViewFmt.load(std::memory_order_acquire) == static_cast<uint32_t>(selectedFormat) &&
        m_cmdAllocators[0] && m_cmdLists[0] && m_fence && m_fenceEvent;
    if (colorResourcesReady && (!wantDepthSwapchains || haveDepthSwapchains)) {
        return true;
    }

    // ADD-DEPTH FAST PATH: the color swapchains/fence/lists are already good and the
    // ONLY thing missing is the depth swapchains (the game's depth was just discovered
    // mid-session, flipping wantDepthSwapchains on). Do NOT fall through to the full
    // teardown below -- destroying the LIVE color swapchains + fence + command lists
    // while GPU frames are in flight (no GPU idle) froze the present thread here (the
    // hang reproduced exactly when depth turned on at ~serial 301, before any
    // [DEPTHDBG] logged). Instead create the depth swapchains ADDITIVELY on the existing
    // eye swapchains -- pure creation touches no in-flight resource, so it can't hang.
    if (colorResourcesReady && wantDepthSwapchains && !haveDepthSwapchains) {
        bool addedOk = true;
        for (size_t eye = 0; eye < m_eyeSwapchains.size(); ++eye) {
            if (m_eyeSwapchains[eye].depthHandle != XR_NULL_HANDLE) continue;
            XrSwapchainCreateInfo depthInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            depthInfo.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
            depthInfo.format = selectedDepthFormat;
            depthInfo.sampleCount = 1;
            depthInfo.width = m_eyeSwapchains[eye].width;
            depthInfo.height = m_eyeSwapchains[eye].height;
            depthInfo.faceCount = 1;
            depthInfo.arraySize = 1;
            depthInfo.mipCount = 1;
            const XrResult dres = xrCreateSwapchain(m_session, &depthInfo, &m_eyeSwapchains[eye].depthHandle);
            if (XR_FAILED(dres)) {
                Log("OpenXRManager: [DEPTH] add-depth: xrCreateSwapchain failed eye %zu (res=%d) -> depth disabled\n", eye, dres);
                m_eyeSwapchains[eye].depthHandle = XR_NULL_HANDLE;
                m_depthLayerSupported = false;
                addedOk = false;
                break;
            }
            uint32_t dImageCount = 0;
            xrEnumerateSwapchainImages(m_eyeSwapchains[eye].depthHandle, 0, &dImageCount, nullptr);
            m_eyeSwapchains[eye].depthImages.resize(dImageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
            xrEnumerateSwapchainImages(m_eyeSwapchains[eye].depthHandle, dImageCount, &dImageCount,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(m_eyeSwapchains[eye].depthImages.data()));
        }
        m_depthSwapchainFormat = addedOk ? selectedDepthFormat : m_depthSwapchainFormat;
        Log("OpenXRManager: [DEPTH] depth swapchains added in-place (no color rebuild) ok=%d fmt=%lld\n",
            addedOk ? 1 : 0, selectedDepthFormat);
        return true;
    }

    // xrDestroySwapchain requires every submitted app graphics command using
    // its images to have completed. Defer resize without blocking either queue.
    // The conversion pass also replaces descriptor heaps/PSO on resize.
    if(!PollSubmitResources(true))return false;
    m_worldFrameReplay.Invalidate();
    if (formatSelection.decodeSrgbToLinear) {
        if (!m_srgbToLinearPass) m_srgbToLinearPass = std::make_unique<SrgbToLinearPass>();
        if (!m_srgbToLinearPass->EnsureInitialized(m_d3dDevice, formatSelection.format, width, height)) {
            Log("OpenXRManager: failed to initialize sRGB-to-linear UNORM fallback\n");
            return false;
        }
    }
    m_decodeSrgbForUnormSwapchain = formatSelection.decodeSrgbToLinear;

    for (auto& eye : m_eyeSwapchains) {
        if (eye.handle != XR_NULL_HANDLE) {
            xrDestroySwapchain(eye.handle);
            eye.handle = XR_NULL_HANDLE;
        }
        if (eye.depthHandle != XR_NULL_HANDLE) {
            xrDestroySwapchain(eye.depthHandle);
            eye.depthHandle = XR_NULL_HANDLE;
        }
    }
    m_eyeSwapchains.clear();

    // Drop the cached last-good textures: a swapchain (re)create may change size/
    // format, which would mismatch CopyResource. They re-create lazily next frame.
    m_lastGoodValid = false;
    for (int e = 0; e < 2; ++e) { m_lastGoodEye[e].Reset(); m_lastGoodEyeInited[e] = false; }

    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
    {
        std::lock_guard lock(m_presentMutex);
        if(!m_submitFenceRetirement.RetireFence(m_fenceValue))return false;
        if (m_fence) {m_fence->Release();m_fence = nullptr;}
    }
    for (int i = 0; i < 3; ++i) {
        if (m_cmdLists[i]) {
            m_cmdLists[i]->Release();
            m_cmdLists[i] = nullptr;
        }
        if (m_cmdAllocators[i]) {
            m_cmdAllocators[i]->Release();
            m_cmdAllocators[i] = nullptr;
        }
    }

    m_eyeSwapchains.resize(viewCount);

    for (uint32_t eye = 0; eye < viewCount; ++eye) {
        XrSwapchainCreateInfo swapchainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        swapchainInfo.format = selectedFormat;
        swapchainInfo.sampleCount = 1;
        swapchainInfo.width = static_cast<int32_t>(eye<2?eyeWidths[eye]:width);
        swapchainInfo.height = static_cast<int32_t>(eye<2?eyeHeights[eye]:height);
        swapchainInfo.faceCount = 1;
        swapchainInfo.arraySize = 1;
        swapchainInfo.mipCount = 1;

        const XrResult res = xrCreateSwapchain(m_session, &swapchainInfo, &m_eyeSwapchains[eye].handle);
        if (XR_FAILED(res)) {
            Log("OpenXRManager: Failed to create mono swapchain for eye %u (res=%d)\n", eye, res);
            return false;
        }

        m_eyeSwapchains[eye].width = swapchainInfo.width;
        m_eyeSwapchains[eye].height = swapchainInfo.height;

        uint32_t imageCount = 0;
        xrEnumerateSwapchainImages(m_eyeSwapchains[eye].handle, 0, &imageCount, nullptr);
        m_eyeSwapchains[eye].images.resize(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        xrEnumerateSwapchainImages(
            m_eyeSwapchains[eye].handle,
            imageCount,
            &imageCount,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(m_eyeSwapchains[eye].images.data()));
        // The runtime creates these, so nothing here has ever named them, and the debug layer
        // therefore reports every complaint about them as 'Unnamed ID3D12Resource Object'. A name
        // is what turned error 527 from 'some texture' into 'eye 1, image 2' the first time.
        for (uint32_t im = 0; im < imageCount && im < m_eyeSwapchains[eye].images.size(); ++im) {
            if (m_eyeSwapchains[eye].images[im].texture)
                SetD3DNamef(m_eyeSwapchains[eye].images[im].texture,
                            L"OpenXR_eye%u_color_image%u", static_cast<unsigned>(eye), im);
        }

        // Publish the eye image's real geometry for the capture thread. It needs the format
        // and size to build its right-eye target, and it cannot look them up here: this vector
        // lives on the submit thread and gets resized out from under any other reader.
        if (eye == 0 && !m_eyeSwapchains[eye].images.empty() &&
            m_eyeSwapchains[eye].images[0].texture) {
            const D3D12_RESOURCE_DESC ed = m_eyeSwapchains[eye].images[0].texture->GetDesc();
            m_eyeImageW.store(static_cast<uint32_t>(ed.Width), std::memory_order_release);
            m_eyeImageH.store(ed.Height, std::memory_order_release);
            m_eyeImageFmt.store(static_cast<uint32_t>(ed.Format), std::memory_order_release);
            // The TYPED companion: what the swapchain was actually created as. The resource
            // above is typeless, and views must be typed.
            m_eyeViewFmt.store(static_cast<uint32_t>(selectedFormat), std::memory_order_release);
            Log("OpenXRManager: eye image %ux%u resourceFmt=%u viewFmt=%u\n",
                static_cast<unsigned>(ed.Width), ed.Height,
                static_cast<unsigned>(ed.Format), static_cast<unsigned>(selectedFormat));
        }

        if (wantDepthSwapchains) {
            XrSwapchainCreateInfo depthInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            depthInfo.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
            depthInfo.format = selectedDepthFormat;
            depthInfo.sampleCount = 1;
            depthInfo.width = m_eyeSwapchains[eye].width;
            depthInfo.height = m_eyeSwapchains[eye].height;
            depthInfo.faceCount = 1;
            depthInfo.arraySize = 1;
            depthInfo.mipCount = 1;
            const XrResult dres = xrCreateSwapchain(m_session, &depthInfo, &m_eyeSwapchains[eye].depthHandle);
            if (XR_FAILED(dres)) {
                Log("OpenXRManager: Failed to create depth swapchain for eye %u (res=%d) — disabling depth layer\n", eye, dres);
                m_eyeSwapchains[eye].depthHandle = XR_NULL_HANDLE;
                m_depthLayerSupported = false;
            } else {
                uint32_t dImageCount = 0;
                xrEnumerateSwapchainImages(m_eyeSwapchains[eye].depthHandle, 0, &dImageCount, nullptr);
                m_eyeSwapchains[eye].depthImages.resize(dImageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
                xrEnumerateSwapchainImages(
                    m_eyeSwapchains[eye].depthHandle,
                    dImageCount,
                    &dImageCount,
                    reinterpret_cast<XrSwapchainImageBaseHeader*>(m_eyeSwapchains[eye].depthImages.data()));
            }
        }
    }
    m_depthSwapchainFormat = selectedDepthFormat;

    char formatSummary[512] = {};
    int summaryPos = sprintf_s(formatSummary,
        "OpenXRManager: Mono swapchain formats. game=%u selected=%lld srgbDecode=%d runtime:",
        format, selectedFormat, formatSelection.decodeSrgbToLinear ? 1 : 0);
    if (summaryPos > 0) {
        for (uint32_t i = 0; i < runtimeFormatCount && summaryPos > 0 && summaryPos < static_cast<int>(sizeof(formatSummary) - 32); ++i) {
            summaryPos += sprintf_s(formatSummary + summaryPos, sizeof(formatSummary) - summaryPos, " %lld", runtimeFormats[i]);
        }
        Log("%s\n", formatSummary);
    }

    for (int i = 0; i < 3; ++i) {
        if (FAILED(m_d3dDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_cmdAllocators[i])))) {
            Log("OpenXRManager: Failed to create submit command allocator %d\n", i);
            return false;
        }
        SetD3DName(m_cmdAllocators[i], L"OpenXR_submit_allocator");
    }
    for (int i = 0; i < 3; ++i) {
        if (FAILED(m_d3dDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_cmdAllocators[i], nullptr, IID_PPV_ARGS(&m_cmdLists[i])))) {
            Log("OpenXRManager: Failed to create submit command list %d\n", i);
            return false;
        }
        SetD3DName(m_cmdLists[i], L"OpenXR_submit_command_list");
        m_cmdLists[i]->Close();
    }

    // Old resources were drained above; all three allocators/lists here are new.
    m_submitAllocatorFences.fill(0);
    if(!CreateSubmitFence())return false;
    SetD3DName(m_fence, L"OpenXR_submit_fence");
    m_fenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) {
        Log("OpenXRManager: Failed to create mono fence event\n");
        return false;
    }

    Log("OpenXRManager: Mono submit resources ready. game=%ux%u eye0=%dx%d rec0=%ux%u format=%u\n",
        width,
        height,
        viewCount != 0 ? m_eyeSwapchains[0].width : 0,
        viewCount != 0 ? m_eyeSwapchains[0].height : 0,
        viewCount != 0 ? m_viewConfigViews[0].recommendedImageRectWidth : 0,
        viewCount != 0 ? m_viewConfigViews[0].recommendedImageRectHeight : 0,
        format);
    return true;
}

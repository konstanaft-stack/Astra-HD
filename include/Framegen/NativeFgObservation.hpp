#pragma once
#include <nvsdk_ngx.h>
#include <d3d12.h>
#include <cstdint>
#include "Framegen/PresentationStatus.hpp"

namespace cvr::framegen {
struct NativeFgReceipt;
bool CopyNativeFgInitialPermission(ID3D12GraphicsCommandList*,ID3D12Resource* destination) noexcept;
void BeginNativeFgCompanionOutput(ID3D12GraphicsCommandList*,ID3D12Resource* input,ID3D12Resource* generated,ID3D12Resource* disable,ID3D12Resource* realOutput) noexcept;
bool EndNativeFgCompanionOutput(ID3D12GraphicsCommandList*) noexcept;
void RecordNativeFgCompanionOutput(ID3D12GraphicsCommandList*,ID3D12Resource* real,ID3D12Resource* generated,
    ID3D12Resource* disable,const NativeFgReceipt&) noexcept;
void EnsureNativeFgPolicyObservation() noexcept;
bool NativeFgPolicyObservationRequested() noexcept;
// Opt-in diagnostics: returns true only for a bounded validation message
// observed inside the original Close of a list used by native FG.
bool TraceNativeFgCloseValidation(uint32_t id,const char* message) noexcept;
bool NativeFgGameEnabled() noexcept;
NativePolicyStatus ReadNativeFgPolicyStatus() noexcept;
bool NativeFgGameWasEnabled() noexcept;
void ObserveNativeFgStereoResult(ID3D12GraphicsCommandList*,const NVSDK_NGX_Handle*,const NativeFgReceipt&) noexcept;
// Observes the existing host's feature 11 call. No parameter or GPU mutations.
struct NativeFgObservationTicket {uint64_t serial{};bool record{};};
NativeFgObservationTicket BeginNativeFgObservation(ID3D12GraphicsCommandList*,const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter*) noexcept;
void EndNativeFgObservation(NativeFgObservationTicket,ID3D12GraphicsCommandList*,const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter*,NVSDK_NGX_Result) noexcept;
void ObserveNativeFgBarriers(ID3D12GraphicsCommandList*,UINT,const D3D12_RESOURCE_BARRIER*) noexcept;
void ResetNativeFgObservation(ID3D12GraphicsCommandList*) noexcept;
// Shared native CopyResource observer; forwards the original once and reports
// provenance only while an original SL resource-tagging call is active.
bool EnsureNativeResourceCopyObservation(ID3D12GraphicsCommandList*) noexcept;
}

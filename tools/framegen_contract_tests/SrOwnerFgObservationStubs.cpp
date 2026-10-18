#include "Framegen/NativeFgObservation.hpp"
// These owner lifecycle fixtures do not install the production NGX observer.
// FG pixel/resource permission is covered separately by NativeFgOutputTests.
namespace cvr::framegen {
bool CopyNativeFgInitialPermission(ID3D12GraphicsCommandList*,ID3D12Resource*) noexcept{return false;}
void BeginNativeFgCompanionOutput(ID3D12GraphicsCommandList*,ID3D12Resource*,ID3D12Resource*,ID3D12Resource*,ID3D12Resource*) noexcept{}
bool EndNativeFgCompanionOutput(ID3D12GraphicsCommandList*) noexcept{return false;}
void RecordNativeFgCompanionOutput(ID3D12GraphicsCommandList*,ID3D12Resource*,ID3D12Resource*,ID3D12Resource*,const NativeFgReceipt&) noexcept{}
void ObserveNativeFgStereoResult(ID3D12GraphicsCommandList*,const NVSDK_NGX_Handle*,const NativeFgReceipt&) noexcept{}
}

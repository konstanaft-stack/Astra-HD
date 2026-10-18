#pragma once
#include "MenuPreview/ImageLineagePolicy.hpp"
#include <d3d12.h>
namespace cvr::menu_preview {
// Save/restore this TLS with the node's exact native context, camera key and clock.
PreviewNodeScopeToken EnterPreviewNodeBinding(uintptr_t nativeContext,uint64_t cameraKey,bool clockKnown,uint32_t nativeFrame,uint32_t workRva=0,uintptr_t existingViewObject=0)noexcept;
void NotePreviewNodeEpilogue(uint32_t workRva)noexcept;
void RestorePreviewNodeBinding(PreviewNodeScopeToken)noexcept;
bool PreviewRtvTrackingActive()noexcept;
void ObservePreviewRtvBind(ID3D12GraphicsCommandList*,ID3D12Resource*);
void ObservePreviewImageBarriers(ID3D12GraphicsCommandList*,UINT,const D3D12_RESOURCE_BARRIER*);
void ResetPreviewImageList(ID3D12GraphicsCommandList*);
using PreviewImageSubmission=ImageLineageStore::Prepared;
using SubmittedPreviewRtvBinding=ImageLineageStore::Image;
PreviewImageSubmission PreparePreviewImageLists(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*)noexcept;
void CommitPreviewImageLists(ID3D12CommandQueue*,PreviewImageSubmission&&)noexcept;
// Exact resource submission lineage for a future SRV consumption probe. Empty
// after unknown bind, scope invalidation, release/reuse or rejected submit.
// Generation is CPU observed Execute order, NOT a completed GPU fence.
SubmittedPreviewRtvBinding ReadSubmittedPreviewRtvBinding(ID3D12Resource*);
// Future consumer holds ImageSubmissionMutex across selection/revalidation and
// its actual queue submission; this checks exact generation and originating queue.
bool ValidateSubmittedPreviewRtvBinding(const SubmittedPreviewRtvBinding&,ID3D12CommandQueue*);
}

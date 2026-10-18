#pragma once
#include <memory>
#include <cstdint>
namespace cvr::menu_preview {
struct UiFrameReceipt;
// Called only after full UI copy execution, successful fence Signal and publication.
void AcknowledgeUiCapture(const UiFrameReceipt&)noexcept;
std::shared_ptr<const UiFrameReceipt> ReadAcknowledgedUiCapture()noexcept;
void NoteUiPairSubmitted(uint32_t epoch,uint64_t leftRevision,uint64_t rightRevision)noexcept;
bool UiPairSubmitted(uint32_t epoch)noexcept;
}

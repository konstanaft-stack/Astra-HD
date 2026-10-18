#include "MenuPreview/UiCameraCycle.hpp"
#include "MenuPreview/UiConsumer.hpp"
#include <atomic>
#include <mutex>
namespace cvr::menu_preview {
namespace {std::atomic<std::shared_ptr<const UiFrameReceipt>> captured;std::mutex mutex;std::atomic<uint32_t> submittedEpoch{};}
void AcknowledgeUiCapture(const UiFrameReceipt& value)noexcept {
    if(!value.scope.screenEpoch||value.eye>1||!value.cameraRevision||!value.uiSubmission)return;
    try {std::lock_guard lock(mutex);const auto old=captured.load(std::memory_order_acquire);
        if(old&&old->uiSubmission>=value.uiSubmission)return;
        captured.store(std::make_shared<const UiFrameReceipt>(value),std::memory_order_release);
    }catch(...){}
}
std::shared_ptr<const UiFrameReceipt> ReadAcknowledgedUiCapture()noexcept{return captured.load(std::memory_order_acquire);}
void NoteUiPairSubmitted(uint32_t epoch,uint64_t leftRevision,uint64_t rightRevision)noexcept{
    if(epoch&&leftRevision&&rightRevision&&leftRevision!=rightRevision)submittedEpoch.store(epoch,std::memory_order_release);
}
bool UiPairSubmitted(uint32_t epoch)noexcept{return epoch&&submittedEpoch.load(std::memory_order_acquire)==epoch;}
}

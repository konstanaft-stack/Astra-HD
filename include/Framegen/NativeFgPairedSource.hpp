#pragma once
#include "Framegen/Presenter.hpp"
#include "Framegen/SrHistoryOwner.hpp"
namespace cvr::framegen {
struct NativeFgPairedSource {
    Frame frame;
    uint64_t gameWorldEpoch{};
    unsigned mainEye=2;
};
// Immutable handoff at actual capture publication, before host FG Evaluate.
// Replacing/clearing must occur outside the capture owner's publication lock.
bool ValidNativeFgPairedSource(const NativeFgPairedSource&);
SrSourceStamp PairedNativeStamp(const NativeEyeStamp&);
bool MatchesPairedNativeStamp(const NativeEyeStamp&,const SrSourceStamp&);
void PublishNativeFgPairedSource(std::shared_ptr<const NativeFgPairedSource>);
std::shared_ptr<const NativeFgPairedSource> AcquireNativeFgPairedSource(const SrSourceStamp& main);
void ClearNativeFgPairedSource();
}

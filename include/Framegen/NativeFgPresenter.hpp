#pragma once
#include "Framegen/AerRealPresenter.hpp"
namespace cvr::framegen {
class NativeFgPresenter {
public:
    NativeFgPresenter();~NativeFgPresenter();
    void Ingest(const AerReadyIngress&);
    bool Select(ID3D12Device*,const AerRealSelection&,PresentationTime,Selection&);
    bool SelectPaired(ID3D12Device*,const Frame& baseline,uint64_t gameWorldEpoch,PresentationTime,Selection&);
    void Submitted(const Selection&,ID3D12Fence*,uint64_t,bool accepted,bool copySubmitted,bool signalSucceeded,int64_t displayTime);
    void Reset();
    // Retire completed XR consumers even when no generated frame is selected.
    // Called on the presenter/XR thread; preserves cadence and source history.
    void Collect();
    uint64_t Epoch()const;
private:struct Impl;std::unique_ptr<Impl> impl;
};
}

#pragma once
#include "gpu.hpp"
#include "avatar.hpp"
namespace cpvr {
struct TrackingSnapshot {
    ViewSample views;
    std::shared_ptr<const AvatarSnapshot> avatar;
    bool same_sample{};
};
// One session owns head/controller tracking; game callbacks only read snapshots.
class TrackingOutput {
public:
    TrackingOutput();
    bool initialize(ID3D12Device*,ID3D12CommandQueue*,Log);
    bool tick(const FrameSelector&,const PanelSelector& = {});
    void shutdown();
    ViewSample views() const {return output_.views();}
    XrOutputStatus status() const {return output_.status();}
    XrClockMapping clock_mapping() const {return output_.clock_mapping();}
    std::shared_ptr<const AvatarSnapshot> avatar() const {return avatar_.latest();}
    std::shared_ptr<const TrackingSnapshot> snapshot() const {return snapshot_.load(std::memory_order_acquire);}
    bool request_body_settings(const BodySettings&s){return avatar_.request_settings(s);}
    void recenter_body(){avatar_.request_recenter();}
private:
    XrOutput output_;
    AvatarTracker avatar_;
    std::atomic<std::shared_ptr<const TrackingSnapshot>> snapshot_;
    void publish_snapshot();
};
}

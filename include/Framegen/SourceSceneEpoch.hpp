#pragma once
#include <cstdint>
#include <limits>

namespace cvr::framegen {
struct SourceSceneFrame {
    uint64_t worldEpoch{},ordinal{};
    uintptr_t scene{};
    bool valid{},paused{};
    bool operator==(const SourceSceneFrame&) const=default;
};
// Single owner receives actual game lifecycle/FrameBegin callbacks. The
// ordinal belongs to this observer, NOT to a guessed field in FrameInfo,
// renderer frame number, source ID, QPC, or OpenXR frame number.
class SourceSceneEpoch {
public:
    void Attach(uintptr_t scene) {
        if(!scene)return;
        Invalidate();
        if(!exhausted)state.scene=scene;
    }
    void Detach(uintptr_t scene) {if(scene && state.scene==scene)Invalidate();}
    void Pause(bool paused) {state.paused=paused;state.valid=false;}
    void Shutdown() {Invalidate();}
    SourceSceneFrame BeginFrame() {
        state.valid=false;
        if(!state.scene||state.paused||exhausted)return state;
        if(state.ordinal==(std::numeric_limits<uint64_t>::max)()) {Invalidate();return state;}
        ++state.ordinal;state.valid=true;return state;
    }
    SourceSceneFrame Snapshot() const {return state;}
    bool RejectFrame(const SourceSceneFrame& failed) {
        if(state!=failed)return false;
        state.valid=false;return true;
    }
private:
    void Invalidate() {
        if(state.worldEpoch==(std::numeric_limits<uint64_t>::max)())exhausted=true;
        const auto epoch=exhausted?0:state.worldEpoch+1;
        state={epoch,0,0,false,false};
    }
    SourceSceneFrame state{};
    bool exhausted{};
};
}

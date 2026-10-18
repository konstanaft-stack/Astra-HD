#pragma once
#include "Framegen/FramePolicy.hpp"
#include "Framegen/Generator.hpp"
#include "Framegen/CaptureCompletion.hpp"
#include "Framegen/NativeFrameStamp.hpp"
#include "Framegen/FrameProjectionMetadata.hpp"
#include "Framegen/OwnSelectionWitness.hpp"
#include <openxr/openxr.h>
#include <optional>

namespace cvr::framegen {
class NativeGenerationBridge;
struct NativeSelectionLease;
struct ReadLease {
    using Release=void (*)(void*,ID3D12Resource*,uint64_t);
    void* owner{};Release release{};
    ID3D12Resource* resources[2]{};
    uint64_t fence{};
    ~ReadLease() {if(release)for(auto* resource:resources)if(resource)release(owner,resource,fence);}
};
struct Frame {
    Identity identity;
    ID3D12Resource* color[2]{};
    uint32_t engineView[2]{};
    XrPosef pose[2]{};
    XrFovf fov[2]{};
    std::shared_ptr<const Inputs> inputs[2];
    std::shared_ptr<ReadLease> lease;
    CaptureCompletion colorCompletion;
    NativeFrameStamp nativeStamp;
    std::optional<FrameProjectionMetadata> projection[2];
};
struct Selection {
    Frame frame;
    Cadence::Next kind=Cadence::Next::Real;
    // Sources read by generation even when its displayed image is synthetic.
    std::shared_ptr<ReadLease> generatedFrom;
    bool synthetic=false;
    std::shared_ptr<NativeSelectionLease> native;
    OwnSelectionWitness ownWitness;
};
class Presenter {
public:
    Selection Select(ID3D12Device*,ID3D12CommandQueue*,Frame,bool gameplay,PresentationTime={});
    void Submitted(const Selection&,ID3D12Fence*,uint64_t,bool accepted,
        bool copySubmitted=true,bool signalSucceeded=true,int64_t displayTime=0);
    // Programmatic owner connection; no dormant backend is added to the UI.
    // Returns false while the author's generator still has GPU consumers.
    bool SetNativeBridge(std::shared_ptr<NativeGenerationBridge>);
    void Reset(const char* reason="presenter");
    bool Pending() const {return cadence.Pending()!=0;}
private:
    Cadence cadence;
    Generator generator;
    Frame pending;
    Frame previous;
    bool primed{};
    uint64_t generatedSerial{},sdkFrame{};
    std::shared_ptr<NativeGenerationBridge> nativeBridge;
};
}

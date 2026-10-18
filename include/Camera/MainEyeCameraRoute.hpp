#pragma once
#include <cstdint>
namespace cvr::camera {
enum class MainEyeCameraRoute:uint8_t {Unknown,Component,Scene};
// Observed ONLY at the native MainRead selected-setup boundary. Serializing an
// unused blend candidate must not switch the route or invalidate its peers.
class MainEyeCameraRouteGate {
public:
    bool Observe(bool requested,MainEyeCameraRoute route){
        if(!requested){Reset();return false;}
        // An unlabelled/stale selected setup is rejected by the caller, but
        // supplies no positive evidence that the physical camera changed.
        if(route==MainEyeCameraRoute::Unknown)return false;
        if(route==current)return false;
        const bool changed=current!=MainEyeCameraRoute::Unknown;
        current=route;return changed;
    }
    void Reset(){current=MainEyeCameraRoute::Unknown;}
private:
    MainEyeCameraRoute current=MainEyeCameraRoute::Unknown;
};
template<class Latch>
bool AdmitSelectedMainEyeRoute(MainEyeCameraRouteGate& gate,Latch& latch,MainEyeCameraRoute route){
    if(gate.Observe(latch.Requested(),route)){latch.Invalidate();return false;}
    return route!=MainEyeCameraRoute::Unknown;
}
}

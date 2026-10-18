#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace cvr::menu_preview {
struct EditorControllerSample {
    int64_t timeNs{};
    uint64_t origin{};
    float grip{},stickX{},stickY{},deadzone=.2f;
    bool available{},blocked{},stickClick{};
    float orbitX{},orbitDeadzone=.2f;
};
struct EditorControllerStep {
    bool claimed{},reset{};
    float height{},distance{},orbit{};
};
class EditorControllerPolicy {
public:
    EditorControllerStep Update(const EditorControllerSample& sample)noexcept;
    void Reset()noexcept{held_=click_=false;time_=0;origin_=0;}
private:
    bool held_{},click_{};
    int64_t time_{};
    uint64_t origin_{};
};
inline float AccumulateEditorOrbit(float angle,float step)noexcept{return step==0?angle:std::remainder(angle+step,6.283185307179586f);}
inline void ConsumeEditorOrbitX(bool claimed,float& controllerX,float& rawX)noexcept{if(claimed){controllerX=0;rawX=0;}}
// Controls remain fixed until a complete stereo pair with this revision has
// actually reached xrEndFrame. A new value starts only on the next left eye.
class EditorControlLatch {
public:
    struct Value {float height{},distance{};uint64_t revision{};float orbit{};};
    Value Resolve(uint64_t source,uint32_t eye,float height,float distance,
                  uint64_t presentedRevision,uint64_t& revisionCounter,float orbit=0)noexcept;
    void Reset()noexcept{value_={};source_=0;}
private:
    Value value_{};
    uint64_t source_{};
};
}

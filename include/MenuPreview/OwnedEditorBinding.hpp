#pragma once
#include "MenuPreview/Scope.hpp"
#include <optional>
#include <span>

namespace cvr::menu_preview {
inline constexpr uint64_t EditorRightKey=0x9e62210faa8c1037ull;
inline constexpr uint64_t EditorRightTexture=0x2058242661b85b3dull;
struct EditorCameraCandidate {
    uintptr_t component{},owner{};uint64_t name{},key{},texture{};bool enabled{},rtt{};
};
inline bool ExactOwnedEditorSecondary(const EditorCameraCandidate& value,uintptr_t owner)noexcept{
    return owner&&value.component&&value.owner==owner&&value.rtt&&value.name==EditorRightKey&&
        value.key==EditorRightKey&&value.texture==EditorRightTexture;
}
struct EditorCameraSelection {uintptr_t primary{},secondary{};uint32_t enabledPrimary{},exactSecondary{};};
inline EditorCameraSelection SelectOwnedEditorCameras(std::span<const EditorCameraCandidate> values,uintptr_t owner)noexcept{
    EditorCameraSelection result;
    for(const auto& value:values){
        if(!value.component||value.owner!=owner||!value.rtt)continue;
        if(ExactOwnedEditorSecondary(value,owner)){++result.exactSecondary;result.secondary=value.component;continue;}
        if(value.enabled){++result.enabledPrimary;result.primary=value.component;}
    }
    return result;
}
inline bool ReadyEditorBackings(uintptr_t left,uintptr_t right)noexcept{return left&&right&&left!=right;}
struct EditorBindingIdentity {
    Metadata common{};NativePreviewMode mode=NativePreviewMode::LegacyOneView;
    uintptr_t secondaryComponent{};uint64_t secondaryKey{};uintptr_t primaryResource{},secondaryResource{};
};
inline bool SameEditorBinding(const EditorBindingIdentity& a,const EditorBindingIdentity& b)noexcept{
    const auto& m=a.common;const auto& n=b.common;
    return m.screenEpoch==n.screenEpoch&&m.setupID==n.setupID&&m.screen==n.screen&&
        m.controllerIdentity==n.controllerIdentity&&m.componentIdentity==n.componentIdentity&&
        m.entityIdentity==n.entityIdentity&&m.entityID==n.entityID&&m.sceneNameHash==n.sceneNameHash&&
        m.cameraRefHash==n.cameraRefHash&&m.slotNameHash==n.slotNameHash&&m.virtualCameraNameHash==n.virtualCameraNameHash&&
        a.mode==b.mode&&a.secondaryComponent==b.secondaryComponent&&a.secondaryKey==b.secondaryKey&&
        a.primaryResource==b.primaryResource&&a.secondaryResource==b.secondaryResource;
}
// Script-thread only. Clock never resets/recycles when a scope is invalidated.
class EditorBindingClock {
public:
    uint64_t Resolve(const EditorBindingIdentity& value)noexcept{
        if(current_&&SameEditorBinding(*current_,value))return generation_;
        if(generation_==std::numeric_limits<uint64_t>::max())return 0;
        ++generation_;current_=value;return generation_;
    }
    void Invalidate()noexcept{current_.reset();}
private:std::optional<EditorBindingIdentity> current_;uint64_t generation_{};
};
}

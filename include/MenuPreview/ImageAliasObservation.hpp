#pragma once
#include "MenuPreview/Scope.hpp"
namespace cvr::menu_preview {
struct ImageAliasObservation {
    Metadata metadata{};uint64_t sequence{},observedAtMs{},nameHash{},resourcePath{};
    uintptr_t backend{},resource{},componentResource{},assignmentSlot{};
    bool scopeKnown{},aliasMatch{};
};
// Copied callback-time identities only. No raw backend is retained for later
// dereference; this trace does not assign a GPU generation or physical eye.
inline std::atomic<std::shared_ptr<const ImageAliasObservation>> imageAliasObservation;
inline std::shared_ptr<const ImageAliasObservation> ReadImageAliasObservation(){return imageAliasObservation.load(std::memory_order_acquire);}
inline bool ExactImageResourceAlias(uintptr_t imageResource,uintptr_t previewResource,bool scopeKnown){return scopeKnown && imageResource && imageResource==previewResource;}
}

#pragma once
#include "MenuPreview/Scope.hpp"
namespace cvr::menu_preview {
struct WidgetAliasObservation {
    Metadata metadata{};uint64_t sequence{},observedAtMs{},externalName{},backendName{};
    uintptr_t widget{},backend{},resource{},componentResource{};
    uintptr_t currentReadBacking{},currentWriteBacking{};
    bool backingFieldsKnown{};
    bool useExternal{},snapshotStable{},aliasMatch{};
};
inline std::atomic<std::shared_ptr<const WidgetAliasObservation>> widgetAliasObservation;
inline std::shared_ptr<const WidgetAliasObservation> ReadWidgetAliasObservation(){return widgetAliasObservation.load(std::memory_order_acquire);}
inline bool ExactWidgetResourceAlias(uintptr_t imageResource,uintptr_t previewResource,bool currentScope,bool stable){return currentScope && stable && imageResource && imageResource==previewResource;}
}

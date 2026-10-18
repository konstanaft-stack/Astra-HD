#pragma once
#include "MenuPreview/WidgetAliasObservation.hpp"
#include "MenuPreview/ProducerRoute.hpp"
namespace cvr::menu_preview {
struct ReaderObservation {
    Metadata metadata{};uint64_t sequence{},observedAtMs{},aliasSequence{};
    uintptr_t resource{},outputRef{},readBacking{},writeBacking{},caller{},nodeWork{};
    uint64_t nodeKey{};uint32_t rendererClock{};
    bool nodeKnown{},clockKnown{},readFieldMatch{};
};
inline std::atomic<std::shared_ptr<const ReaderObservation>> readerObservation;
inline std::shared_ptr<const ReaderObservation> ReadReaderObservation(){return readerObservation.load(std::memory_order_acquire);}
inline bool ExactReaderAlias(const WidgetAliasObservation& alias,const Metadata& current,uintptr_t resource){
    return alias.aliasMatch && alias.snapshotStable && resource && resource==alias.resource &&
        resource==alias.componentResource && SamePreview(alias.metadata,current) &&
        alias.metadata.controllerIdentity==current.controllerIdentity;
}
}

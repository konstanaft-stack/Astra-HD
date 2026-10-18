#pragma once
#include <cstdint>
#include <utility>
namespace cvr::menu_preview {
enum class UiQualifiedSourceAction:uint8_t {Reject,Keep,Replace};
inline UiQualifiedSourceAction DecideUiQualifiedSource(bool nativeDual,bool sameScope,
    uintptr_t source,uint64_t generation,uintptr_t next,uint64_t nextGeneration)noexcept {
    if(!sameScope||!next||!nextGeneration)return UiQualifiedSourceAction::Reject;
    if(source==next&&generation==nextGeneration)return UiQualifiedSourceAction::Keep;
    return nativeDual?UiQualifiedSourceAction::Replace:UiQualifiedSourceAction::Reject;
}
inline bool UiQualifiedSourceEventMatches(uintptr_t source,uint64_t generation,
    uintptr_t eventSource,uint64_t eventGeneration)noexcept {
    return source&&generation&&source==eventSource&&generation==eventGeneration;
}
template<class AppOwner,class UiOwner>struct UiRetiredQualifiedOwners {AppOwner app{};UiOwner ui{};};
// Proof retirement only. Existing event/GPU/display owners are independent.
// Caller releases the returned owners after leaving its observer mutex.
template<class Target,class Scope>
auto ReplaceUiQualifiedTargetSource(Target& target,uintptr_t source,uint64_t generation,const Scope& scope){
    UiRetiredQualifiedOwners<decltype(target.qualifiedApp),decltype(target.editorUi)> retired{
        std::move(target.qualifiedApp),std::move(target.editorUi)};
    const auto resource=target.resource,resourceGeneration=target.generation;
    target={};target.resource=resource;target.generation=resourceGeneration;
    target.source=source;target.sourceGeneration=generation;target.sourceScope=scope;
    return retired;
}
}

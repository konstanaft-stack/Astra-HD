#pragma once
#include <cstdint>
namespace cvr::menu_preview {
enum class UiEditorDrawKind:uint8_t {Outside,Unknown,Portrait,Panel};
struct EditorTargetKey {
 uint64_t epoch{},setup{},controller{},list{},listLife{},target{},targetLife{},node{};
 bool operator==(const EditorTargetKey&)const=default;
};
inline bool EditorTargetMatches(const EditorTargetKey& learned,const EditorTargetKey& current)noexcept{
 return learned.epoch&&learned.controller&&learned.list&&learned.listLife&&learned.target&&learned.targetLife&&learned.node&&learned==current;
}
// CPU binding proof only. Unknown inputs never constitute a negative proof.
struct EditorDrawEvidence {
 bool inside{},targetLearned{},shaderKnown{},locationsKnown{};
 uint32_t used{},nonAlias{},alias{};
 bool unknown{},conflict{};
};
inline UiEditorDrawKind ClassifyEditorDraw(const EditorDrawEvidence& e)noexcept{
 if(!e.inside)return UiEditorDrawKind::Outside;
 if(!e.targetLearned||!e.shaderKnown||!e.locationsKnown||e.conflict)return UiEditorDrawKind::Unknown;
 if(e.alias)return UiEditorDrawKind::Portrait;
 if(e.unknown||e.nonAlias!=e.used)return UiEditorDrawKind::Unknown;
 return UiEditorDrawKind::Panel;
}
}

#pragma once
namespace cvr::xr {
// An absent preview scope alone says nothing about being in the editor.
// Once observed, its open epoch covers category gaps until authoritative exit.
inline constexpr bool EditorUiFallbackBlock(bool enabled,bool selected,bool freshEditor,
    bool liveEpochOpen,bool anchorEpochOpen)noexcept{
    return enabled&&!selected&&(freshEditor||liveEpochOpen||anchorEpochOpen);
}
}

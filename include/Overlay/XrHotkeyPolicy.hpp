#pragma once
#include <cstdint>
#include <string_view>

namespace cvr::overlay_hotkeys {
enum class Action { None, ToggleFramegen, TogglePortMenu };
struct Context {
    bool fgOwner{}, optiMenuOwner{}, xrRunning{}, windowFocused{}, modified{}, nativeFgStereo{};
    int backend{};
};

constexpr bool ExactOwner(std::string_view value) { return value == "1"; }

constexpr Action Resolve(std::uint32_t key, bool release, bool system, const Context& state) {
    if (!release) return Action::None;
    if (key == 0x79) return Action::TogglePortMenu; // Preserve F10, including system-key release.
    if (key == 0x2D) return state.optiMenuOwner ? Action::None : Action::TogglePortMenu;
    if (key == 0x7B && !system && !state.modified && state.fgOwner && state.xrRunning &&
        state.windowFocused && !state.nativeFgStereo && (state.backend == 1 || state.backend == 2))
        return Action::ToggleFramegen;
    return Action::None;
}
}

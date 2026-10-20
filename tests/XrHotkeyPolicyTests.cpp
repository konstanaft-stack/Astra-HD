#include <cstdint>
#include <iostream>
#include <string_view>

#if __has_include("Overlay/XrHotkeyPolicy.hpp")
#include "Overlay/XrHotkeyPolicy.hpp"
#else
// RED control: the actual legacy OverlayWndProc release decision, before this
// feature exists. Ownership and XR flags did not affect these two bindings.
namespace cvr::overlay_hotkeys {
enum class Action { None, ToggleFramegen, TogglePortMenu };
struct Context {
    bool fgOwner{}, optiMenuOwner{}, xrRunning{}, windowFocused{}, modified{}, nativeFgStereo{};
    int backend{};
};
constexpr bool ExactOwner(std::string_view value) { return value == "1"; }
constexpr Action Resolve(std::uint32_t key, bool release, bool, const Context&) {
    return release && (key == 0x79 || key == 0x2D) ? Action::TogglePortMenu : Action::None;
}
}
#endif

int main() {
    using namespace cvr::overlay_hotkeys;
    unsigned checks{}, failures{};
    const auto check = [&](bool ok, const char* name) {
        ++checks;
        if (!ok) { ++failures; std::cerr << "FAIL " << name << '\n'; }
    };
    for (const auto value : {"", "0", "01", "10", "true", "1 ", " 1"})
        check(!ExactOwner(value), "only exact process-owner value1 is accepted");
    check(ExactOwner("1"), "exact owner1 accepted");
    check(!ExactOwner(std::string_view("1\0x", 3)), "embedded suffix not accepted");

    Context qualified{true, true, true, true, false, false, 1};
    check(Resolve(0x7B, true, false, qualified) == Action::ToggleFramegen,
          "owned focused running builtin1 F12 release toggles existing generator");
    auto value = qualified; value.backend = 2;
    check(Resolve(0x7B, true, false, value) == Action::ToggleFramegen, "own2 uses same toggle");
    check(Resolve(0x7B, false, false, qualified) == Action::None, "key-down and auto-repeat do not toggle");
    check(Resolve(0x7B, true, true, qualified) == Action::None, "system key-up is not plain F12");
    value = qualified; value.modified = true;
    check(Resolve(0x7B, true, false, value) == Action::None, "modified F12 is ignored");
    value = qualified; value.fgOwner = false;
    check(Resolve(0x7B, true, false, value) == Action::None, "legacy process leaves F12 alone");
    value = qualified; value.xrRunning = false;
    check(Resolve(0x7B, true, false, value) == Action::None, "closed XR leaves F12 alone");
    value = qualified; value.windowFocused = false;
    check(Resolve(0x7B, true, false, value) == Action::None, "other foreground window cannot toggle");
    value = qualified; value.nativeFgStereo = true;
    check(Resolve(0x7B, true, false, value) == Action::None, "native game FG retains its separate owner");
    for (const int backend : {0, 3, -1}) {
        value = qualified; value.backend = backend;
        check(Resolve(0x7B, true, false, value) == Action::None, "unsupported backend is unchanged");
    }
    check(Resolve(0xDC, true, false, qualified) == Action::None, "NR backslash never belongs to this policy");
    check(Resolve(0x2D, true, false, qualified) == Action::None, "Opti owner suppresses only port Insert binding");
    value = qualified; value.optiMenuOwner = false;
    check(Resolve(0x2D, true, false, value) == Action::TogglePortMenu, "legacy Insert is preserved");
    value = {}; value.optiMenuOwner = true; value.nativeFgStereo = true;
    check(Resolve(0x2D, true, true, value) == Action::None, "menu ownership is independent of XR and native FG");
    check(Resolve(0x79, true, true, value) == Action::TogglePortMenu, "F10 retains legacy system-release binding");
    check(Resolve(0x79, false, false, value) == Action::None, "F10 down does not toggle");
    check(Resolve(0x2D, false, false, value) == Action::None, "Insert down does not toggle");
    std::cout << "checks=" << checks << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}

# Astra HD

[![Русский](https://img.shields.io/badge/Русский-2563eb?style=for-the-badge)](README.ru.md)
[![English](https://img.shields.io/badge/English-2563eb?style=for-the-badge)](README.md)
[![中文](https://img.shields.io/badge/中文-2563eb?style=for-the-badge)](README.zh-CN.md)

No mode is declared finished. Features, controls and architecture may change or disappear following testing; this is not a promised roadmap or complete feature list.

## VR-CENTR + Astra HD: install and use together

### Development goal

We want Cyberpunk 2077 to become a complete VR experience: from spatial menus and the character editor to a comfortable, readable HUD and interaction with the game world. The foundation is high-quality stereo, stable FPS and coherent neural rendering with frame generation. The interface should suit headsets and VR controllers. This is our development goal, not a list of finished features.

This is an unfinished experimental combination, not a finished stable VR port. Astra HD sends the game to the headset; VR-CENTR manages installation contents and profiles. Profiles were created because repeated VR, mod and library tests left mixed, nonworking file combinations. They support repeatable tests and restoring a chosen state without manually deleting several game folders, downloading the game again through Steam or reinstalling after each test. 

**No ready combined installation archive is available yet.** [Astra HD Releases](https://github.com/konstanaft-stack/Astra-HD/releases) is the future download location. The automatic Source ZIP contains source code, not an installer; a checkout alone does not restore a working installation.

Once a verified installation archive is published:

1. Install your own Cyberpunk 2077 through Steam. The public package does not include the game, paid third-party ports or personal saves.
2. Download the ready archive from Releases, extract it, run its included installer and select your game folder.
3. In VR-CENTR, first preserve a verified clean baseline and create a clean profile. Then choose a mod collection and Astra HD VR port, and apply that composition.
4. Launch through the center. Switch profiles, ports or libraries only after closing the game; save any current state you want to keep first.

The shared OptiScaler/NR/MFG package is installed once per game, not separately per profile. Back up personal saves separately. HUD, editor, motion and performance still need headset validation.


### Current experiments and development directions

- **Dual camera:** separate left/right views are the current foundation. FPS, frame generation and stability remain under development; 80 FPS is not verified.
- **Alternating eyes/AER:** the user subjectively finds the world more natural, freer and more present than with our current dual-camera output; the cause is unknown. This mode is seriously unfinished: large halos previously diminished or disappeared, then returned. That regression remains unresolved.
- **Stereo character editor:** spatial preview, headset inspection and left grip/stick camera height/zoom are being developed. Centering, UI overlap and FPS defects remain; this is not an accepted, finished Luke-like mode.

Contributions investigating a specific defect or branch are welcome through PRs. Include version, GPU, HMD/runtime and reproduction steps. Upstream attribution is preserved.

### Current prototype controls: editor camera

Hold left grip: left stick ↑↓ adjusts height (0.5 m/s), ←→ adjusts distance (0.6 m/s). Grip+left-stick click resets camera height/distance only, not global HMD recentering. Mouse: Shift+wheel height, Ctrl+wheel distance, Ctrl+middle button reset. Headset validation of these axes remains open.

OptiScaler: Insert opens its menu, F12 requests its FG toggle, and \ toggles NR after world load. These are separate from port Own FG. On the current RTX4060 setup, NR310.9.16 still returns createFeatureNotSupported.

Forks, porting our changes to other forks or bringing Cyberpunk support into a main project, and help with individual defects are welcome. We continue our own development alongside this collaboration; preserve licenses and upstream attribution.

This update publishes documentation only. A ready download, a complete current-source backup and an installer are not supplied by this commit.

## Origins and credits

- [dariulone/cyberpunk-vr-port](https://github.com/dariulone/cyberpunk-vr-port): original project and direct upstream.
- [dabinn/cyberpunk-vr-port — Tofu Express](https://github.com/dabinn/cyberpunk-vr-port): an important basis for our local development.
- Other community contributions will be attributed individually when incorporated.

Original history and author notices are preserved. Existing components retain their respective licenses.

Updated: 18 October 2026.

[15 October 2026: stereo editor checkpoint, image and focused code change](STEREO-EDITOR-2026-10-15.md)

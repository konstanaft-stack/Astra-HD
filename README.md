# Astra HD

[![Русский](https://img.shields.io/badge/Русский-2563eb?style=for-the-badge)](README.ru.md)
[![English](https://img.shields.io/badge/English-2563eb?style=for-the-badge)](README.md)
[![中文](https://img.shields.io/badge/中文-2563eb?style=for-the-badge)](README.zh-CN.md)

**Cyberpunk 2077 VR · Active development**

Astra HD is an experimental fork of [dariulone/cyberpunk-vr-port](https://github.com/dariulone/cyberpunk-vr-port). Our goal is a comfortable VR playthrough with coherent stereo images, clear visuals, and interfaces suited to a headset.

> **There is no stable Astra HD release yet.** This public repository currently contains the upstream base and project documentation. Astra-specific changes are being developed and validated locally; inherited upstream files are not a verified Astra HD distribution.

## Development and validation

| Area | Current status |
| --- | --- |
| Gameplay output to both eyes | Verified in local OpenXR simulator sessions; broader headset acceptance remains open |
| Inventory character stereo | Visually verified in the local simulator |
| Appearance editor stereo | Both eye outputs visually verified in the OpenXR simulator on 15 October; physical-headset acceptance pending |
| NVIDIA frame-generation integration | Verified in selected local scenarios; performance, transitions, and compatibility remain under test |
| Settings persistence | Startup resets remain unresolved |
| Stable full-game playthrough | Not yet verified |

The current focus is character-editor stereo, reliable image output, frame generation, and settings persistence. Later work will address HUD placement, navigation, weapon selection, and other VR interactions.

Simulator results support development. Comfort, motion, image quality, and performance must also be checked in a physical headset. Installation instructions and Astra HD binaries will accompany a verified release.

## Origins and credits

- [dariulone/cyberpunk-vr-port](https://github.com/dariulone/cyberpunk-vr-port): original project and direct upstream.
- [dabinn/cyberpunk-vr-port — Tofu Express](https://github.com/dabinn/cyberpunk-vr-port): an important basis for our local development.
- Other community contributions will be attributed individually when incorporated.

Original history and author notices are preserved. Existing components retain their respective licenses.

Updated: 15 October 2026.

[15 October 2026: stereo editor checkpoint, image and focused code change](STEREO-EDITOR-2026-10-15.md)

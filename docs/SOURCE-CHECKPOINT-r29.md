# Source checkpoint for r29 / Center04

This source snapshot is overlaid on the existing public Astra-HD history, not
force-pushed from the unrelated development repository.

- C4 tracked source checkpoint: `bbfa2ec325426bf26deec251a0f6c1e52b6d6679`.
- Accepted runtime: r29/full22, renderer
  `570D757645AAD308F2429D63B5B71F9C412D9F68E14C25BFC99DB8448158E4CD`.
- Center04 source: the172-file reviewed source export under `vr-center/`,
  retaining its own LICENSE, third-party notices and `SOURCE-MANIFEST.json`.
- Native worker: five translation units and25 headers checked against the
  accepted full22 input snapshot. Backend source checkpoint and external header
  pins are in `native/SOURCE-PINS.json`.

Runtime/frame logic is copied from the committed C4 checkpoint. Publication-only
adaptations set the default generation-worker directory to `./native`, make the
editor asset generator's audit output root relative to this checkout, and remove
the original workstation's paths from developer test/diagnostic examples.
`<SOURCE_ROOT>`, `<WORKSPACE_ROOT>` and `<USER_HOME>` in those developer adapters
are explicit local-configuration placeholders. Pure C++ test bodies are retained;
this source preparation did not execute or claim portability for every adapter.

Two compressed vanilla player-entity JSON fixtures are deliberately omitted.
The related tests require the user to obtain their own permitted matching game
inputs; complete game data is not a public source fixture. The existing upstream
authored mod-resource tree and credits remain separate from that exclusion.

No installed game, personal profile, save, credential, local audit, build output
or NVIDIA runtime was imported into this source checkpoint. The ten proprietary
NGX headers needed by the backend are external inputs rather than MIT source.
This is source for review/continuation; the runtime installation and known
unfinished behavior are documented in the root INSTALL and STATUS pages.

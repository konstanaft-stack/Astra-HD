# World-space image probe

Opt-in CET diagnostic source. Not installed by default. It does not change PPD,
graphics settings, camera selection, frame generation, or persistent entities.

The fixture contains a 25 cm grid, closely spaced vertical edges, diagonal
edges, and a foreground wire cube. Its anchor is captured once from the player
camera and stored in `anchor.json`; subsequent head movement does not move the
fixture. `replay` reuses that anchor. A moving cube uses a four-second period.

After CET has initialized the mod, write a new request to `command.json`:

```json
{"id": 1, "action": "arm", "distance": 3, "moving": false}
```

Use a new ID for each request. Other actions are `replay` and `stop`. Distances
are metres, range 1–12. An existing command present at process startup is ignored
to avoid silently enabling the fixture in a later normal play session.

`status.json` records activation, anchor, draw-call count, and explicit errors.
A successful API call is not proof that either eye received visible geometry.
Player unload or drawing failure disables the fixture until a new command.

## Verification boundary

LuaJIT tests cover syntax, command lifecycle, fixed anchoring, replay, stop,
player unload and error handling. No native game run has been performed yet.
The engine's treatment of a zero line lifetime, both-eye inclusion, and the
debug draw's position relative to temporal processing must be checked in-game.

These are **unlit debug lines**, not a lit-material/reflection test scene.
They must not be used to declare SSR, denoising, DLSS or AER halos fixed. The
material test needs actual engine meshes with reflective and matte materials,
a controlled light, and matched per-eye temporal captures. A final-output-only
grid also cannot establish that an upstream processing stage is correct.

CPU-only check:

```powershell
python tools/framegen_contract_tests/image_probe_lua.py --lua-deps <isolated-lupa-directory>
```

# PPD camera catalogue regression

Run the offline generator tests from the repository root:

```powershell
python -X utf8 -m unittest discover -s tools/ppd_tests -p '*_test.py' -v
```

The compressed fixtures are exact WolvenKit 9.0.1 JSON exports of two player
entities from the original working ToFu archive. Their archive and uncompressed
JSON hashes are recorded in `fixtures/provenance.json`. They cover 179 and 190
original components, each with 62 VRCAM cameras. No game, GPU, WolvenKit install,
or third-party Python package is needed for these tests.

The regression catches the former 655-size expansion, which added 645 cameras
to the original 62 and produced 813–835 components. At startup those cameras
created transform attachments on the player's `slots` component. The observed
collector has 256 output pointers followed by its count, so pointer 257
overwrites that count. Cameras being disabled did not avoid their attachments.

The generator conservatively limits total authored components to 256, including
the non-camera components, and checks every available entity before writing the
first one. This is an asset policy for the observed attachment collector, not a
universal engine component limit or proof about later runtime attachments.

To prepare one additional size, start a **fresh staging project from the original
bounded archive**, copy its binaries into `source/archive`, and export all four
player and four replacer entities to their depot paths under `source/raw`.
Supply an explicit copy of the original `vrcam.json`:

```powershell
python tools/gen_vrcam_assets.py --project STAGE --catalog STAGE/vrcam.json --selected-resolution 2320x2584
```

This preserves all 62 original sizes, appends the selected camera and dtex, and
selects its matching name in the staged catalogue. Serialize the changed JSON,
replace only those eight entity binaries and the new dtex in the staged original
tree, and pack/re-extract/verify before integrating the package. Do not reuse an
old expanded 707-camera tree or accumulate every possible PPD size.

This command only prepares an offline package. It does not resize resources in
a running game, reset history, or establish Applied PPD. Live Adapt still needs
the source/SR/eye resource transition and actual descriptor/epoch confirmation.

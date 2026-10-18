# Native generation worker and NVIDIA backend source

This directory contains the generation worker source used by the r29/full22 C4
build and the maintained own C ABI NVIDIA backend source associated with the
CB33 backend. `SOURCE-PINS.json` identifies the source checkpoints and the ten
separately required NGX headers. The original project attribution is preserved
in `THIRD_PARTY_NOTICES.txt`; the repository's upstream LICENSE is unchanged.

The source is supplied for review and continuation. This publication preparation
did not independently rebuild either the compiler or the runtime DLLs.

## Build boundary

The root CMake project defaults `CVR_GENERATION_ROOT` to this directory. It builds
the five worker translation units and uses `include/cpvr/`. Existing C4
submodules retain their pinned commits and must be initialized through Git.

The backend's proprietary NVIDIA NGX headers are not included or relicensed
under MIT. Obtain a separately licensed applicable NVIDIA NGX/DLSS-G SDK/header
set and compare the ten exact SHA256 values in `SOURCE-PINS.json`; place those
headers under `nvidia_backend/snapshot/include/` only after obtaining the
applicable permission. A mismatched header set is not asserted to reproduce
CB33. NVIDIA runtime DLLs and model files are not source dependencies included
here and retain their separate distribution terms.

`nvidia_backend/Build-Backend.ps1` accepts an explicit `-Zig` compiler path and
defaults to `zig` on PATH. The maintained snapshot is already present;
`Prepare-Backend.ps1` preserves its current identity/loader contracts and must
not be used to regenerate from an older donor. See the source checkpoint note
for the publication path adaptations and exact original source pins.

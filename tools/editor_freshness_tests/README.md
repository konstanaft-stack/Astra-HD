# Bounded editor freshness diagnostics

This standalone CPU target checks scalar recorder behavior. It does not open a game, load a GPU runtime, inspect a headset, or prove image quality.

Configure and build this directory with CMake, then run CTest. The target needs a C++20 compiler and the standard thread library. The recorder test does not require OpenXR headers.

The runtime probe is opt-in through the diagnostic child process environment: `CPVR_EDITOR_FRESHNESS_TRACE=1`. It arms on editor scope/publication, emits at most 32 `[EDITOR-FRESH]` rows, and then stops. Startup/world frames do not spend the row budget. Logging uses the XR terminal path; renderer admission, resource ownership and fence checks stay unchanged.

Rows contain interval event counts and the last observed scalar tuple for each stage. A tuple can belong to a previous interval or epoch. Publication source sequence and submitted scene serial are different identities. A row is not a headset frame, and the probe does not measure physical scanout FPS. Interpret it alongside the existing FramegenReport and the owned run's PID/runtime/DLL evidence.

The separate camera policy test supplies a disabled test adapter. Runtime adapter ON/OFF checks and full render translation-unit builds are distinct verification steps.

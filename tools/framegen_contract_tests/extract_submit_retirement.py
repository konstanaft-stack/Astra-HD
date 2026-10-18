"""Extract the exact production lifetime boundaries for the WARP fixture."""
from pathlib import Path
import hashlib, json, sys

root, out = map(Path, sys.argv[1:3])
out.mkdir(parents=True, exist_ok=True)
source = (root / "src/Runtimes/OpenXRCapture.cpp").read_text(encoding="utf-8")
header = (root / "include/Runtimes/OpenXRManager.hpp").read_text(encoding="utf-8")

def scope(text, start):
    opened = text.index("{", start)
    depth = 1
    i = opened + 1
    while depth:
        depth += (text[i] == "{") - (text[i] == "}")
        i += 1
    return text[start:i]

release = scope(source, source.index("void OpenXRManager::ReleaseCapturedRead("))
parts = [release]
for method in ("CollectCapturedReadsLocked", "PollSubmitResources", "RecordSubmitExecuted", "CreateSubmitFence"):
    found = source.find("OpenXRManager::" + method + "(")
    if found >= 0:
        parts.append(scope(source, source.rfind("\n", 0, found) + 1))
retired = scope(header, header.index("struct RetiredCaptureRead")) + ";"

# The additive-depth branch ends immediately before the full teardown. Keep
# every production statement through list/allocator/fence retirement, including
# a newly added gate; do not reconstruct its behavior in the test.
anchor = source.index('Log("OpenXRManager: [DEPTH] depth swapchains added in-place')
start = source.index("    }\n", source.index("        return true;", anchor)) + 6
end = source.index("    m_eyeSwapchains.resize(viewCount);", start)
teardown = source[start:end]
if "bool OpenXRManager::CreateSubmitFence()" in source:
    create = "    return CreateSubmitFence();"
else:
    start = source.index("    if (FAILED(m_d3dDevice->CreateFence(", end)
    end = source.index('    SetD3DName(m_fence, L"OpenXR_submit_fence");', start)
    create = source[start:end]

files = {"submit_methods.inc": "\n\n".join(parts),
         "submit_retired.inc": retired,
         "submit_teardown.inc": teardown,
         "submit_create.inc": create}
for name, text in files.items():
    (out / name).write_text(text + "\n", encoding="utf-8")
has_tracker = (root / "include/Runtimes/SubmitFenceRetirement.hpp").exists()
(out / "submit_config.inc").write_text("#define HAS_SUBMIT_TRACKER " + str(int(has_tracker)) + "\n", encoding="utf-8")
manifest = {"source": str(root), "tracker": has_tracker, "fragments": {
    name: hashlib.sha256(text.encode()).hexdigest() for name, text in files.items()}}
if has_tracker:
    loop = (root / "src/Runtimes/OpenXRFrameLoop.cpp").read_text(encoding="utf-8")
    owner = (root / "src/Runtimes/OpenXRManager.cpp").read_text(encoding="utf-8")
    ensure = scope(source, source.index("bool OpenXRManager::EnsureMonoSubmitResources()"))
    shutdown = scope(owner, owner.index("void OpenXRManager::Shutdown()"))
    gates = {
        "unknown_gate_before_resources": ensure.index("PollSubmitResources(false)") < ensure.index("colorResourcesReady"),
        "completion_before_pass_reinit": ensure.index("PollSubmitResources(true)") < ensure.index("m_srgbToLinearPass->EnsureInitialized"),
        "completion_before_xr_destroy": ensure.index("PollSubmitResources(true)") < ensure.index("xrDestroySwapchain("),
        "shutdown_completion_before_xr_destroy": shutdown.index("PollSubmitResources(true)") < shutdown.index("xrDestroySwapchain("),
        "single_app_xr_execute": loop.count("m_d3dQueue->ExecuteCommandLists(") == 1,
        "submission_record_before_read_receipt": loop.index("RecordSubmitExecuted(capturedCopySignaled)") < loop.index("capturedReadFence=m_fenceValue"),
        "late_callback_never_reads_raw_fence": "m_fence" not in release,
    }
    manifest["integration_guards"] = gates
    assert all(gates.values()), gates
(out / "extracted.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
print("Exact production fragments extracted; tracker:", has_tracker)

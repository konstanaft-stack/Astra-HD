"""Check sustained accepted FG progress in real game telemetry, not XR repeats.

Regression: a busy output allocator used to select a product, reject its submit,
and reset generation history every tick. This gate uses real runtime samples;
it does not inspect source text or emulate the implementation. Passing this
counter gate still requires separate stereo-pixel and physical-HMD inspection.
"""
import argparse
import datetime as dt
import json
from pathlib import Path


def check(path):
    segment = []
    selected = []
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        item = json.loads(line)
        report = item.get("report", {})
        valid = (report.get("enabled") == 1 and
                 report.get("presented_camera") == 2 and
                 len(report.get("inputs", [])) == 2 and
                 all(value > 0 for value in report["inputs"]))
        if not valid:
            segment = []
            continue
        stamp = dt.datetime.fromisoformat(item["time"])
        identity = (item["identity"]["pid"], item["identity"]["start_utc"],
                    item["module"]["sha256"])
        if segment and (identity != segment[-1][1] or
                        not 0 < (stamp - segment[-1][0]).total_seconds() <= 2.5):
            segment = []
        segment.append((stamp, identity, report))
        # Five seconds of initial history, then an uninterrupted 30-sample gate.
        if len(segment) == 35:
            selected = segment[5:]
            break
    if not selected:
        return {"status": "NOT_RUN", "reason": "No stable 35-sample paired-world interval"}, 2
    reports = [sample[2] for sample in selected]
    counters = [report["generated"] for report in reports]
    progress = counters[-1] - counters[0]
    active = sum(report["fg_fps"] > 0 and report["active"] for report in reports)
    monotonic = all(b >= a for a, b in zip(counters, counters[1:]))
    # Poll timestamps alone do not prove that the exported report is fresh.
    # Reject one initial burst followed by a frozen, still-positive FPS export.
    block_progress = [counters[min(start + 5, len(counters) - 1)] - counters[start]
                      for start in range(0, len(counters) - 1, 5)]
    passed = monotonic and all(delta > 0 for delta in block_progress) and active >= 24
    return {
        "status": "PASS" if passed else "FAIL",
        "scope": "Accepted generation counter progress only; stereo pixels and HMD remain separate",
        "pid": selected[0][1][0], "process_start": selected[0][1][1],
        "dll_sha256": selected[0][1][2],
        "start": selected[0][0].isoformat(), "end": selected[-1][0].isoformat(),
        "samples": len(reports), "active_fg_samples": active,
        "generated_delta": progress, "counter_monotonic": monotonic,
        "generated_block_deltas": block_progress,
        "real_fps_range": [min(r["real_fps"] for r in reports), max(r["real_fps"] for r in reports)],
        "fg_fps_range": [min(r["fg_fps"] for r in reports), max(r["fg_fps"] for r in reports)],
        "reason": "Sustained generation observed" if passed else "Paired world renders but generation does not sustain progress",
    }, 0 if passed else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--samples", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if args.out.exists() or not args.out.parent.is_dir():
        raise RuntimeError("Use a fresh output path in an existing audit directory")
    result, code = check(args.samples)
    args.out.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result))
    raise SystemExit(code)

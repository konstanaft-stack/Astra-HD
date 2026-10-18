"""Compile the actual observer body in a controlled NVIDIA API host fixture.

No copied policy implementation: each generation reads the current production
function, so the fixture detects changes in the real forwarding path.
"""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[2]
source = (root / 'src/Framegen/NativeFgPolicyObservation.cpp').read_text(encoding='utf-8-sig')
start = source.index('sl::Result ObserveOptions(')
end = source.index('sl::Result ObserveState(', start)
body = source[start:end].rstrip()
poll_start = source.index('void EnsureNativeFgPolicyObservation()')
poll_end = source.index('bool NativeFgPolicyObservationRequested()', poll_start)
poll = source[poll_start:poll_end].rstrip()
template = Path(__file__).with_name('native_options_observer.cpp.in').read_text(encoding='utf-8')
assert template.count('@@PRODUCTION_OBSERVER@@') == 1
output = Path(sys.argv[1])
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(template.replace('@@PRODUCTION_OBSERVER@@', body).replace('@@PRODUCTION_POLICY_POLL@@', poll), encoding='utf-8')
print(output)

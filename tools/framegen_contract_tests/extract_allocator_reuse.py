"""Compile-test the real XR allocator wait/reset statements, including skipped frames."""
from pathlib import Path
import sys,hashlib,json
root,out=map(Path,sys.argv[1:3]);out.mkdir(parents=True,exist_ok=True)
p=root/'src/Runtimes/OpenXRFrameLoop.cpp';s=p.read_text(encoding='utf-8')
a=s.index('m_cmdAllocatorIndex = (m_cmdAllocatorIndex + 1) % 3;')
b=s.index('// ---- the second eye',a)
wait=s[a:b]
a=s.index('if (monoSource && monoHasView[0] && monoHasView[1] &&',b)
b=s.index('{',a);condition=s[a:b]
(out/'allocator_step.inc').write_text(wait+'\n'+condition+'{++recorded;}\n',encoding='utf-8')
(out/'extracted.json').write_text(json.dumps({'source':str(p),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()},indent=2))

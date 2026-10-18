import pathlib, sys
sys.path.insert(0, r'<WORKSPACE_ROOT>/research/2026-09-30-cpvr-open-nvidia/tooling/menu-preview/test-deps')
from lupa.luajit21 import LuaRuntime
root = pathlib.Path(__file__).resolve().parent.parent
source_path = pathlib.Path(sys.argv[1]) if len(sys.argv)>1 else root / 'mods/cet/CyberpunkVRPort_Stereo/modules/menu_preview.lua'
source = source_path.read_text(encoding='utf-8-sig')
fixture = (root / 'tests/MenuPreviewCategoryLifecycle.lua').read_text(encoding='utf-8-sig')
lua = LuaRuntime(unpack_returned_tuples=True)
print(lua.execute(fixture)(source))

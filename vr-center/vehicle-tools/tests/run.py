from pathlib import Path
import sys
import os

ROOT = Path(__file__).resolve().parents[1]
LUPA = os.environ.get("VRPORTS_LUA_FIXTURE")
if LUPA:
    sys.path.insert(0, str(Path(LUPA).expanduser().resolve()))

from lupa.luajit21 import LuaRuntime

lua = LuaRuntime(unpack_returned_tuples=True)
lua.globals().VEHICLE_INIT_SOURCE = (
    ROOT / "CyberpunkVRPort_VehicleGrant" / "init.lua"
).read_text(encoding="utf-8")
lua.execute((ROOT / "tests" / "test.lua").read_text(encoding="utf-8"))
print("PASS: LuaJIT 2.1 syntax and CET mock session/vehicle behavior")
